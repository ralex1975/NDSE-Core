#include "PhysMem.h"

#define RGB(r,g,b) (r | (g << 8) | (b << 16))

memory_region< PAGING >          memory::null_region("(NULL)",                0x0000FF00, 0);
memory_region< PAGING >          memory::hle_bios("HLE BIOS", RGB(199,199,23), 1);        // 0xEFEF0000


memory_region< PAGING::KB<64> >  memory::accessory_ram("DS Accessory RAM",    0x00000000, 1); 
memory_region< PAGING::KB<32> >  memory::accessory_rom("DS Accessorry ROM",   0x00000000, 1); // 32MB?
memory_region< PAGING::KB<2> >   memory::oam_ab("2D Graphics Engine A/B OAM", 0x00000000, 1);
memory_region< PAGING::KB<128> > memory::obj_vram_b("2D Graphics Engine B OBJ-VRAM", 0x00000000, 1);
memory_region< PAGING::KB<256> > memory::obj_vram_a("2D Graphics Engine A OBJ-VRAM", 0x00000000, 1);
memory_region< PAGING::KB<128> > memory::bg_vram_b("2D Graphics Engine B BG-VRAM",   0x00000000, 1);
memory_region< PAGING::KB<512> > memory::bg_vram_a("2D Graphics Engine A BG-VRAM",   0x00000000, 1);
memory_region< PAGING::B<2048> > memory::palettes("Palette Memory",                  0x00000000, 1);

// io registers
memory_region< PAGING::KB<32> >  memory::arm7_shared("Shared Internal Work RAM", 0x00000000, 1);
memory_region< PAGING::KB<16> >  memory::data_tcm("Data TCM",                    0x00C000C0, 1000);
memory_region< PAGING::B<0x3E0000> > memory::ram("Main Memory",                  0x00FF0000, 1);
memory_region< PAGING::KB<32> >  memory::inst_tcm("Instruction TCM",             0x00C0C000, 1001);
memory_region< PAGING::KB<256> > memory::exp_wram("Internal Expanded Work RAM",  0x00000000, 1);
// wireless communication wait state 1
// wireless communication wait state 0
memory_region< PAGING::KB<64> >  memory::arm7_wram("ARM7 Exclusive Internal Work RAM", 0x00000000, 1);
memory_region< PAGING::B<0x10400> >  memory::system_rom("System ROM", 0x00000000, 1);

memory_region< PAGING::KB<8> >   memory::registers1("IO registers 0", 0x00000000, 1); // 0x04000000
memory_region< PAGING::B<512> >  memory::registers2("IO registers 1", 0x00000000, 1); // 0x04001000
memory_region< PAGING::B<512> >  memory::registers3("IO registers 2", 0x00000000, 1); // 0x04100000

memory_region< PAGING::B<4096> >  memory::cart_header("CART Header", RGB(138,236,170), 1); // 0x027FF000

// VRAM banks
memory_region< PAGING::KB<128> > memory::vram_a("VRAM-A", 0x00000000, 2);
memory_region< PAGING::KB<128> > memory::vram_b("VRAM-B", 0x00000000, 3);
memory_region< PAGING::KB<128> > memory::vram_c("VRAM-C", 0x00000000, 4);
memory_region< PAGING::KB<128> > memory::vram_d("VRAM-D", 0x00000000, 5);
memory_region< PAGING::KB< 64> > memory::vram_e("VRAM-E", 0x00000000, 6);
memory_region< PAGING::KB< 16> > memory::vram_f("VRAM-F", 0x00000000, 7);
memory_region< PAGING::KB< 16> > memory::vram_g("VRAM-G", 0x00000000, 8);
memory_region< PAGING::KB< 32> > memory::vram_h("VRAM-H", 0x00000000, 9);
memory_region< PAGING::KB< 16> > memory::vram_i("VRAM-I", 0x00000000, 10);

const memory_region_base* memory::regions[memory::NUM_REGIONS] = { 
	&accessory_ram,
	&accessory_rom,

	&vram_a,
	&vram_b,
	&vram_c,
	&vram_d,
	&vram_e,
	&vram_f,
	&vram_g,
	&vram_h,
	&vram_i,

	&oam_ab,
	&obj_vram_b,
	&obj_vram_a,
	&bg_vram_b,
	&bg_vram_a,
	&palettes,
	&arm7_shared,
	&data_tcm,
	&ram,
	&inst_tcm,
	&exp_wram,
	&arm7_wram,
	&system_rom,
	&registers1,
	&cart_header
};