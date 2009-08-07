#include "io_observe.h"
#include "Logging.h"
#include "MemMap.h"
#include "fifo.h"
#include "Processor.h"
#include "Interrupt.h"
#include "vram.h"

#if 0

io_observer::reactor_page io_observer::reactor_data9[REGISTERS9_1::PAGES];
io_observer::reactor_page io_observer::reactor_data7[REGISTERS7_1::PAGES];



union endian_access
{
	unsigned long  w;
	unsigned short h[2];
	unsigned char  b[4];
};


void io_observer::process9()
{
}

void io_observer::process7()
{
	/*
	bool remap_v = false;
	for (unsigned int i = 0; i < memory::registers7_1.pages; i++)
	{
		memory_block &b = memory::registers7_1.blocks[i];
		reactor_page &p = reactor_data7[i];
		if (!b.react())
			continue;

		// pull the new data
		unsigned long *bmem = (unsigned long*)&b.mem;
		unsigned long addr_base = i * PAGING::SIZE;
		for (unsigned int j = 0; j < PAGING::SIZE/sizeof(unsigned long); j++)
		{
			if (p[j] == bmem[j])
				continue;

			const char *name = "<unknown>";
			unsigned long addr = addr_base + j * sizeof(unsigned long);
			
			switch (addr)
			{
			case 0x188:
				{
				name = "IPC FIFO Send";
				assert(!fifo<_ARM9>::full());

				fifo<_ARM9>::write(bmem[j]);
				if (fifo<_ARM9>::full())
					bmem[j-1] |= 2; // flag FIFO cnt if full
				unsigned long &remote_fifocnt = reactor_data9[0][0x184 >> 2];
				remote_fifocnt &= ~1; // flag arm9 about FIFO containing data

				// trigger fifo interrupt on ARM9 when requested
				*(unsigned long*)memory::arm9_ipc.blocks[0].mem = fifo<_ARM9>::top();
				interrupt<_ARM9>::fire(18); // IPC Recv FIFO Not Empty
				break;
				}
			case 0x1C0:
				{
					name = "[SPICNT/SPIDATA] SPI Bus Control/Data";

					endian_access &spi = *(endian_access*)(bmem+j);
					
					//unsigned long device = (spi.h[0] >> 8) & 3;
					//unsigned long data   = spi.h[1];
					//unsigned long chan = (data >> 4) & 7;
					

					//logging<_DEFAULT>::logf("SPDATA = %04X on device %i, chan = %i", spi.h[1], device, chan);
					spi.h[1] = (unsigned short)rand();

					break;
				}
			}
			p[j] = bmem[j];
		}
	}
	if (remap_v)
		vram::remap();
	*/
}

void io_observer::io_write9(memory_block*)
{
	process9();
}

void io_observer::io_read9(memory_block *b, unsigned long addr)
{
	unsigned long addr_low = addr & PAGING::ADDRESS_MASK;

	// addr_low [0..0x200) minimum boundaries
	switch (addr_low)
	{
	case 0x184: // IPC FIFO Control Register
		/*
		update following bits:
  8     R    Receive Fifo Empty          (0=Not Empty, 1=Empty)
  9     R    Receive Fifo Full           (0=Not Full, 1=Full)
		*/
		{
			unsigned long &fifocnt = *(unsigned long*)(b->mem + addr_low);
			if (fifo<_ARM9>::empty())
				fifocnt |= 1 << 8;
			else
			{
				fifocnt &= ~(1 << 8);
				if (fifo<_ARM9>::full())
					fifocnt |= 1 << 9;
				else fifocnt &= ~(1 << 9);
			}
			break;
		}
	}
}

void io_observer::io_readipc9(memory_block *b, unsigned long addr)
{
	unsigned long addr_low = addr & PAGING::ADDRESS_MASK;
	if (addr_low >= 0x4)
		DebugBreak_();

	unsigned long &ipc = *(unsigned long*)(b->mem);
	ipc = fifo<_ARM9>::read();
}

void io_observer::io_write7(memory_block*)
{
	process7();
}



void io_observer::init()
{
	memset( reactor_data9, 0, sizeof(reactor_data9) );
	memset( reactor_data7, 0, sizeof(reactor_data7) );
	//reactor_data[0][0x180 >> 2] = 0xEFEFEFEF; // fifo fake sync init

	// register direct callbacks needed to properly sync
	// arm9 pages init
	for (unsigned long i = 0; i < memory::registers9_1.pages; i++)
	{
		memory_block &b = memory::registers9_1.blocks[i];
		b.writecb = io_write9;
		b.readcb = io_read9;
	}

	memory::arm9_ipc.blocks[0].readcb = io_readipc9;

	// arm7 pages init
	for (unsigned long i = 0; i < memory::registers7_1.pages; i++)
	{
		memory_block &b = memory::registers7_1.blocks[i];
		b.writecb = io_write7;
	}

	//static io_observer i;
}

#endif

