#include "IORegs.h"
#include "Logging.h"
#include "Compiler.h" // for DebugBreak_
#include "MemMap.h"
#include "vram.h"
#include "dma.h"

//#define SILENT

// todo: outsource name lookup to an array ....


union endian_access
{
	unsigned long  w;
	unsigned short h[2];
	unsigned char  b[4];
};


REGISTERS9_1::REGISTERS9_1( const char *name, unsigned long color, unsigned long priority )
	: memory_region(name, color, priority)
{
	for (int i = 0; i < PAGES; i++)
		blocks[i].flags |= memory_block::PAGE_ACCESSHANDLER;
}

extern void start_dma3();
void REGISTERS9_1::store32(unsigned long addr, unsigned long value)
{
	memory_block *b = memory_map<_ARM9>::addr2page(addr);
	unsigned long subaddr = addr & (PAGING::ADDRESS_MASK & (~3));
	unsigned long &current = *(unsigned long*)(&b->mem[subaddr]);
	const char *name = 0;
	bool nolog = false;
	bool remap_vram = false;

	switch (addr & 0x1FFF)
	{
		/*
	case 0x00:
		name ="[DISPCNT] 2D Graphics Engine A display control";
		break;
	case 0x04:
		name = "[DISPSTAT/VCOUNT] Display Status / V count comparison";
		break;
	case 0x08:
		name = "[BG0CNT/BB1CNT] 2D Graphics Engine A BG0/BG1 control";
		break;
	case 0x0C:
		name = "[BG2CNT/BB3CNT] 2D Graphics Engine A BG2/BG3 control";
		break;
	case 0x10:
		name = "[BG0HOFS/BG0VOFS] 2D Graphics Engine A BG0 offsets";
		break;
	case 0x30:
		name = "[BG3PA/BG3PB] 2D Graphics Engine A BG3 affine transformation (dx/dmx)";
		break;
	case 0x34:
		name = "[BG3PC/BG3PD] 2D Graphics Engine A BG3 affine transformation (dy/dmy)";
		break;
	case 0x38:
		name = "[BG3X] 2D Graphics Engine A BG3 reference start point (x coordinate)";
		break;
	case 0x3C:
		name = "[BG3Y] 2D Graphics Engine A BG3 reference start point (y coordinate)";
		break;
		*/


	case 0xB0:
		name = "[DMA0SAD] DMA0 source address";
		break;
	case 0xB4:
		name = "[DMA0DAD] DMA0 destination address";
		break;
	case 0xB8:
		name = "[DMA0CNT] DMA0 control";
		current = value;
		start_dma((unsigned long*)(&b->mem[0xB0]));
		return;
	case 0xBC:
		name = "[DMA1SAD] DMA1 source address";
		break;
	case 0xC0:
		name = "[DMA1DAD] DMA1 destination address";
		break;
	case 0xC4:
		name = "[DMA1CNT] DMA1 control";
		current = value;
		start_dma((unsigned long*)(&b->mem[0xBC]));
		return;
	case 0xC8:
		name = "[DMA2SAD] DMA2 source address";
		break;
	case 0xCC:
		name = "[DMA2DAD] DMA2 destination address";
		break;
	case 0xD0:
		name = "[DMA2CNT] DMA2 control";
		current = value;
		start_dma((unsigned long*)(&b->mem[0xC8]));
		return;
	case 0xD4:
		name = "[DMA3SAD] DMA3 source address";
		break;
	case 0xD8:
		name = "[DMA3DAD] DMA3 destination address";
		break;
	case 0xDC:
		name = "[DMA3CNT] DMA3 control";
		current = value;
		start_dma((unsigned long*)(&b->mem[0xD4]));
		return;
	case 0xE0:
		name = "[DMA0FILL] DMA 0 fill data";
		break;
	case 0xE4:
		name = "[DMA1FILL] DMA 1 fill data";
		break;
	case 0xE8:
		name = "[DMA2FILL] DMA 2 fill data";
		break;
	case 0xEC:
		name = "[DMA3FILL] DMA 3 fill data";
		break;

	/*
	case 0x180:
		{
			name = "IPC Synchronize Register";
			// sync to arm7 IPC
			unsigned long *cur = (unsigned long*)(memory::registers7_1.start->mem + 0x180);
			unsigned long ipc = ipc_swizzle(*cur, bmem[j]);
			*cur = ipc;
			reactor_data7[0][0x180 >> 2] = ipc;
			//fake_ipc_sync();
			break;
		}
		*/
		/*
	case 0x184:
		name = "IPC Fifo Control Register";
		break;
	case 0x1C0:
		name = "[SPICNT] SPI Bus Control/Status Register";
		break;
	case 0x204:
		name = "[EXMEMCNT] External memory control";
		break;
		*/

	// 16bit only according to NITRO
	case 0x208:
		name = "[IME] Interrupt master flag";
		nolog = true;
		break;

	case 0x210:
		name = "[IE] Interrupt enable flag";
		nolog = true;
		break;
	case 0x214:
		name = "[IF] Interrupt request flag";
		nolog = true;
		break;
	case 0x240:
		name = "[VRAMCNT] RAM bank control 0";
		remap_vram = true;
		break;
	case 0x244:
		name = "[WRAMCNT] RAM bank control 1";
		remap_vram = true;
		break;
	case 0x444:
		name = "[MTX_PUSH] Push current matrix to stack";
		break;
	case 0x1000:
		name = "[DB_DISPCNT] 2D Graphics Engine B display control";
		break;
	default:
		/*
		logging<_DEFAULT>::logf("Store 32 not supported at %08X [unknown]", addr);
		DebugBreak_();
		*/
		name = "<Unknown>";
	}

#ifndef SILENT
	if (!nolog)
	{
		logging<_DEFAULT>::logf("IO change at ARM9:%08X from %08X to %08X [%s]", 
			addr, current, value, name);
	}
#endif
	if (current != value)
	{
		current = value;
		if (remap_vram)
			vram::remap();
		b->dirty();
	}
}

void REGISTERS9_1::store16(unsigned long addr, unsigned long value)
{
	memory_block *b = memory_map<_ARM9>::addr2page(addr);
	unsigned long subaddr = addr & (PAGING::ADDRESS_MASK & (~1));
	unsigned short &current = *(unsigned short*)(&b->mem[subaddr]);
	const char *name = 0;
	bool nolog = false;
	bool remap_vram = false;

	switch (addr & 0x1FFF)
	{
	case 0x4:
		name = "[DISPSTAT] Display status";
		break;

	case 0xB8:
	case 0xBA:
	case 0xC4:
	case 0xC6:
	case 0xD0:
	case 0xD2:
	case 0xDC:
	case 0xDE:
		{
			endian_access e = *(endian_access*)(&b->mem[subaddr]);
			e.h[(addr >> 1) & 1] = (unsigned short)value;
			return store32(addr & ~3, e.w);
		}


	case 0x100:
		name = "[TM0CNT_L] Timer 0 counter";
		break;
	case 0x102:
		name = "[TM0CNT_H] Timer 0 control";
		break;
	case 0x104:
		name = "[TM1CNT_L] Timer 1 counter";
		break;
	case 0x106:
		name = "[TM1CNT_H] Timer 1 control";
		break;
	case 0x108:
		name = "[TM2CNT_L] Timer 2 counter";
		break;
	case 0x10A:
		name = "[TM2CNT_H] Timer 2 control";
		break;
	case 0x10C:
		name = "[TM3CNT_L] Timer 3 counter";
		break;
	case 0x10E:
		name = "[TM3CNT_H] Timer 3 control";
		break;
	case 0x180:
		name = "[IPCSYNC] IPC synchronize register";
		break;
	case 0x184:
		name = "[IPCFIFOCNT] IPC FIFO control";
		break;
	case 0x208:
		{
			endian_access e = *(endian_access*)(&b->mem[subaddr]);
			e.h[(addr >> 1) & 1] = (unsigned short)value;
			return store32(addr & ~3, e.w);
		}
	

	case 0x248:
		name = "[VRAM_HI_CNT] RAM bank control 2";
		remap_vram = true;
		break;
	case 0x304:
		name = "[POWCNT] Power control";
		break;
	case 0x1008:
		name = "[DB_BG0CNT] 2D Graphics Engine B BG0 control";
		break;
	case 0x1010:
		name = "[DB_BG0HOFS] 2D Graphics Engine B BG0 display H offset";
		break;
	case 0x1012:
		name = "[DB_BG0HOFS] 2D Graphics Engine B BG0 display V offset";
		break;
	default:
		//logging<_DEFAULT>::logf("Store 16 not supported at %08X", addr);
		//DebugBreak_();
		name = "<Unknown>";
	}
#ifndef SILENT
	if (!nolog)
	{
		logging<_DEFAULT>::logf("IO change at ARM9:%08X from %04X to %04X [%s]", 
			addr, current, value, name);
	}
#endif
	if (current != (unsigned short)value)
	{
		current = (unsigned short)value;
		if (remap_vram)
			vram::remap();
		b->dirty();
	}
}

void REGISTERS9_1::store8(unsigned long addr, unsigned long value)
{
	memory_block *b = memory_map<_ARM9>::addr2page(addr);
	unsigned long subaddr = addr & (PAGING::ADDRESS_MASK);
	unsigned char &current = *(unsigned char*)(&b->mem[subaddr]);
	//const char *name = 0;
	bool nolog = false;

	switch (addr & 0x1FFF)
	{
	case 0xB8:
	case 0xB9:
	case 0xBA:
	case 0xBB:

	case 0xC4:
	case 0xC5:
	case 0xC6:
	case 0xC7:
	
	case 0xD0:
	case 0xD1:
	case 0xD2:
	case 0xD3:
	
	case 0xDC:
	case 0xDD:
	case 0xDE:
	case 0xDF:

	case 0x240:
	case 0x241:
	case 0x242:
	case 0x243:

	case 0x244:
	case 0x245:
	case 0x246:
	case 0x247:
		{
			endian_access e = *(endian_access*)(&b->mem[subaddr & (~3)]);
			e.b[addr & 3] = (unsigned char)value;
			return store32(addr & ~3, e.w);
		}

	case 0x248:
	case 0x249:
		{
			endian_access e = *(endian_access*)(&b->mem[subaddr & (~1)]);
			e.b[addr & 1] = (unsigned char)value;
			return store16(addr & ~1, e.w);
		}


	default:
		logging<_DEFAULT>::logf("Store 8 not supported at %08X", addr);
		DebugBreak_();
	}
#ifndef SILENT
	if (!nolog)
	{
		logging<_DEFAULT>::logf("IO change at ARM9:%08X from %02X to %02X [%s]", 
			addr, current, value, name);
	}
#endif
	if (current != (unsigned char)value)
	{
		current = (unsigned char)value;
		b->dirty();
	}

	
}

void REGISTERS9_1::store32_array(unsigned long addr, int /*num*/, unsigned long * /*data*/)
{
	logging<_DEFAULT>::logf("Store Multiple not supported at %08X", addr);
	DebugBreak_();
}

unsigned long REGISTERS9_1::load32(unsigned long addr)
{
	memory_block *b = memory_map<_ARM9>::addr2page(addr);
	unsigned long subaddr = addr & (PAGING::ADDRESS_MASK & (~3));
	unsigned long &current = *(unsigned long*)(&b->mem[subaddr]);
	const char *name = 0;
	bool nolog = true;

	switch (addr & 0x1FFF)
	{
	case 0xDC:
		name = "[DMA3CNT] DMA3 control";
		break;
	case 0x208: // 16bit according to NITRO
		name = "[IME] Interrupt master flag";
		break;
	case 0x210:
		name = "[IE] Interrupt enable flag";
		break;
	case 0x214:
		name = "[IF] Interrupt request flag";
		break;
	case 0x1000:
		name = "[DB_DISPCNT] 2D Graphics Engine B display control";
		break;
	default:
		//logging<_DEFAULT>::logf("Load 32 not supported at %08X", addr);
		//DebugBreak_();
		//return current;
		name = "<Unknown>";
	}
#ifndef SILENT
	if (!nolog)
	{
		logging<_DEFAULT>::logf("IO read of ARM9:%08X = %08X [%s]", 
			addr, current, name);
	}
#endif
	return current;
}

unsigned long REGISTERS9_1::load16u(unsigned long addr)
{
	memory_block *b = memory_map<_ARM9>::addr2page(addr);
	unsigned long subaddr = addr & (PAGING::ADDRESS_MASK & (~3));
	unsigned short &current = *(unsigned short*)(&b->mem[subaddr]);
	const char *name = 0;
	bool nolog = true;

	switch (addr & 0x1FFF)
	{
	case 0x4:
		{
			name = "[DISPSTAT] Display status";
			unsigned short res = current;
			current &= ~1; // hack to break busy idle loops
			return res;
		}
	case 0x180:
		name = "[IPCSYNC] IPC synchronize register";
		break;
	case 0x208:
		name = "[IME] Interrupt master flag";
		break;
	default:
		//logging<_DEFAULT>::logf("Load 16 not supported at %08X", addr);
		//DebugBreak_();
		name = "<Unknown>";
	}

#ifndef SILENT
	if (!nolog)
	{
		logging<_DEFAULT>::logf("IO read of ARM9:%08X = %04X [%s]", 
			addr, current, name);
	}
#endif
	return current;
}

unsigned long REGISTERS9_1::load16s(unsigned long addr)
{
	return (signed short)REGISTERS9_1::load16u(addr);
}

unsigned long REGISTERS9_1::load8u(unsigned long addr)
{
	memory_block *b = memory_map<_ARM9>::addr2page(addr);
	unsigned long subaddr = addr & (PAGING::ADDRESS_MASK & (~3));
	unsigned char &current = *(unsigned char*)(&b->mem[subaddr]);
	const char *name = 0;
	bool nolog = true;

	switch (addr & 0x1FFF)
	{
	case 0x240:
	case 0x241:
	case 0x242:
	case 0x243:
		name = "[VRAMCNT] RAM bank control 0";
		break;
	default:
		//logging<_DEFAULT>::logf("Load 8 not supported at %08X", addr);
		//DebugBreak_();
		name = "<Unknown>";
	}

#ifndef SILENT
	if (!nolog)
	{
		logging<_DEFAULT>::logf("IO read of ARM9:%08X = %042 [%s]", 
			addr, current, name);
	}
#endif

	return current;
}

void REGISTERS9_1::load32_array(unsigned long addr, int /*num*/, unsigned long * /*data*/)
{
	logging<_DEFAULT>::logf("Load Multiple not supported at %08X", addr);
	DebugBreak_();
}
