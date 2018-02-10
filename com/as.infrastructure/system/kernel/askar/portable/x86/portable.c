/**
 * AS - the open source Automotive Software on https://github.com/parai
 *
 * Copyright (C) 2017  AS <parai@foxmail.com>
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation; See <http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
/* ============================ [ INCLUDES  ] ====================================================== */
#include "kernel_internal.h"
#include "asdebug.h"
/* ============================ [ MACROS    ] ====================================================== */
#define AS_LOG_OS 0

/* GDT */
/* 描述符索引 */
#define	INDEX_DUMMY			0	// ┓
#define	INDEX_FLAT_C		1	// ┣ LOADER 里面已经确定了的.
#define	INDEX_FLAT_RW		2	// ┃
#define	INDEX_VIDEO			3	// ┛
#define	INDEX_TSS			4
#define	INDEX_LDT_FIRST		5
/* 选择子 */
#define	SELECTOR_DUMMY		   0		// ┓
#define	SELECTOR_FLAT_C		0x08		// ┣ LOADER 里面已经确定了的.
#define	SELECTOR_FLAT_RW	0x10		// ┃
#define	SELECTOR_VIDEO		(0x18+3)	// ┛<-- RPL=3
#define	SELECTOR_TSS		0x20		// TSS. 从外层跳到内存时 SS 和 ESP 的值从里面获得.
#define SELECTOR_LDT_FIRST	0x28

#define	SELECTOR_KERNEL_CS	SELECTOR_FLAT_C
#define	SELECTOR_KERNEL_DS	SELECTOR_FLAT_RW
#define	SELECTOR_KERNEL_GS	SELECTOR_VIDEO
/* ============================ [ TYPES     ] ====================================================== */

/* ============================ [ DECLARES  ] ====================================================== */
extern void init_prot(void);
extern void init_descriptor(mmu_descriptor_t * p_desc, uint32_t base, uint32_t limit, uint16_t attribute);
extern uint32_t seg2phys(uint16_t seg);
extern void init_clock(void);
extern void restart(void);
extern void dispatch(void);
static void sys_dispatch(void);

/* ============================ [ DATAS     ] ====================================================== */
uint8_t             gdt_ptr[6]; /* 0~15:Limit  16~47:Base */
mmu_descriptor_t    gdt[GDT_SIZE];
uint8_t             idt_ptr[6]; /* 0~15:Limit  16~47:Base */
mmu_gate_t          idt[IDT_SIZE];

uint32_t disp_pos;
uint32_t k_reenter;

tss_t tss;
void* sys_call_table[] = {
	sys_dispatch,
};
/* ============================ [ LOCALS    ] ====================================================== */
static void sys_dispatch(void)
{
	RunningVar = ReadyVar;
	restart();
}
/* ============================ [ FUNCTIONS ] ====================================================== */
void Os_PortActivate(void)
{
	/* get internal resource or NON schedule */
	RunningVar->priority = RunningVar->pConst->runPriority;

	ASLOG(OS, "%s(%d) is running\n", RunningVar->pConst->name,
			RunningVar->pConst->initPriority);

	OSPreTaskHook();

	CallLevel = TCL_TASK;
	Irq_Enable();

	RunningVar->pConst->entry();

	/* Should not return here */
	TerminateTask();
}

void Os_PortInit(void)
{
	int i;
	uint16_t selector_ldt = INDEX_LDT_FIRST << 3;
	for(i=0;i<TASK_NUM;i++){
		asAssert((selector_ldt>>3) < GDT_SIZE);
		init_descriptor(&gdt[selector_ldt>>3],
				vir2phys(seg2phys(SELECTOR_KERNEL_DS), TaskVarArray[i].context.ldts),
				LDT_SIZE * sizeof(mmu_descriptor_t) - 1,
				DA_LDT);
		selector_ldt += 1 << 3;
	}
}

void Os_PortInitContext(TaskVarType* pTaskVar)
{
	uint16_t selector_ldt	= SELECTOR_LDT_FIRST+pTaskVar-TaskVarArray;
	uint8_t privilege;
	uint8_t rpl;
	int	eflags;
	privilege	= PRIVILEGE_TASK;
	rpl		= RPL_TASK;
	eflags = 0x1202; /* IF=1, IOPL=1, bit 2 is always 1 */

	pTaskVar->context.ldt_sel	= selector_ldt;
	memcpy(&pTaskVar->context.ldts[0], &gdt[SELECTOR_KERNEL_CS >> 3], sizeof(mmu_descriptor_t));
	pTaskVar->context.ldts[0].attr1 = DA_C | privilege << 5;	/* change the DPL */
	memcpy(&pTaskVar->context.ldts[1], &gdt[SELECTOR_KERNEL_DS >> 3], sizeof(mmu_descriptor_t));
	pTaskVar->context.ldts[1].attr1 = DA_DRW | privilege << 5;/* change the DPL */
	pTaskVar->context.regs.cs		= ((8 * 0) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | rpl;
	pTaskVar->context.regs.ds		= ((8 * 1) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | rpl;
	pTaskVar->context.regs.es		= ((8 * 1) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | rpl;
	pTaskVar->context.regs.fs		= ((8 * 1) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | rpl;
	pTaskVar->context.regs.ss		= ((8 * 1) & SA_RPL_MASK & SA_TI_MASK) | SA_TIL | rpl;
	pTaskVar->context.regs.gs		= (SELECTOR_KERNEL_GS & SA_RPL_MASK) | rpl;
	pTaskVar->context.regs.eip	= (uint32_t)Os_PortActivate;
	pTaskVar->context.regs.esp	= (uint32_t)(pTaskVar->pConst->pStack + pTaskVar->pConst->stackSize-4);
	pTaskVar->context.regs.eflags	= eflags;

	pTaskVar->context.regs.eax = (uint32_t)pTaskVar;
}

void Os_PortSysTick(void)
{
	unsigned int savedLevel = CallLevel;

	CallLevel = TCL_ISR2;
	OsTick();
	SignalCounter(0);
	CallLevel = savedLevel;

	/* TODO: no dispatch here immediately here,
	 * The Idle task will call Schedule to dispatch high ready .*/
}

void Os_PortStartDispatch(void)
{
	static int flag = 0;
	if(0 == flag)
	{
		flag = 1;
		init_clock();
		RunningVar = ReadyVar;
		Irq_Enable();
		restart();
	}

	Irq_Enable();
	dispatch();
	/* should never return */
	asAssert(0);
}

void Os_PortDispatch(void)
{
	Irq_Enable();
	dispatch();
	Irq_Disable();
}
void cstart(void)
{
	disp_pos = 0;
	ASLOG(OS,"cstart begins\n");

	/* copy the GDT of LOADER to the new GDT */
	memcpy(&gdt,    /* New GDT */
		   (void*)(*((uint32_t*)(&gdt_ptr[2]))),   /* Base  of Old GDT */
		   *((uint16_t*)(&gdt_ptr[0])) + 1    /* Limit of Old GDT */
		);
	/* gdt_ptr[6] has 6 bytes : 0~15:Limit  16~47:Base, acting as parameter of instruction sgdt & lgdt */
	uint16_t* p_gdt_limit = (uint16_t*)(&gdt_ptr[0]);
	uint32_t* p_gdt_base  = (uint32_t*)(&gdt_ptr[2]);
	*p_gdt_limit = GDT_SIZE * sizeof(mmu_descriptor_t) - 1;
	*p_gdt_base  = (uint32_t)&gdt;

	/* idt_ptr[6] 共 6 个字节：0~15:Limit  16~47:Base。用作 sidt 以及 lidt 的参数。*/
	uint16_t* p_idt_limit = (uint16_t*)(&idt_ptr[0]);
	uint32_t* p_idt_base  = (uint32_t*)(&idt_ptr[2]);
	*p_idt_limit = IDT_SIZE * sizeof(mmu_gate_t) - 1;
	*p_idt_base = (uint32_t)&idt;

	init_prot();

	/* 填充 GDT 中 TSS 这个描述符 */
	memset(&tss, 0, sizeof(tss));
	tss.ss0		= SELECTOR_KERNEL_DS;
	init_descriptor(&gdt[INDEX_TSS],
			vir2phys(seg2phys(SELECTOR_KERNEL_DS), &tss),
			sizeof(tss) - 1,
			DA_386TSS);
	tss.iobase	= sizeof(tss);	/* 没有I/O许可位图 */

	ASLOG(OS,"cstart finished\n");
}
