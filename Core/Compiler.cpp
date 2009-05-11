#include <sstream>
#include <list>
#include <assert.h>

#include "Disassembler.h"
#include "ArmContext.h"
#include "Compiler.h"
#include "MemMap.h"
#include "Logging.h"
#include "HLE.h"

#include "osdep.h"

/*
[23:49] <sgstair> arm7 docs show STM writes out the base at the end of the second cycle of the instruction, and a LDM always overwrites the updated base if the base is in the list.
[23:50] <sgstair> so if the base is the lowest register in the list, STM writes the unchanged base, otherwise it writes the final base.
*/

////////////////////////////////////////////////////////////////////////////////
// TODO: sometime in the future
//
// some more performant way would be using immediate calls
// and jumps between codeblocks.
// however this would require that each block holds information
// about all all jump/call instructions to it, so they can be
// relocated.
// recompiling a page would relocate all those jumps
//
// Advantages => no more page / destination lookups
//            => no more indirect jumps
// Disadv     => could break emulation in such a scenario:
//               code gets executed at an address and a mirrored address
//               however the relative jump out could go to different
//               locations
//               => this needs extra work
//
// Memory accesses could be optimized the same way

char NOT_SUPPORTED_YET_BUT_SKIP = '\x90';



// mov to declaration if booting from THUMB is needed
/*
#pragma warning(push)
#pragma warning(disable:4731)
void compiled_block<IS_ARM>::emulate(unsigned long subaddr, emulation_context &ctx)
{
	char *start = remap[subaddr >> T::INSTRUCTION_SIZE_LG2];
	FlushInstructionCache( GetCurrentProcess(), start, code_size - (start - code) );
	__asm
	{
		mov eax, start
		mov edx, ctx
		pushad		
		sub esp, 4096    // reserve some stack that hopefully wont get overwritten

		mov ebp, edx
		call eax // GO!

		add esp, 4096    // restore stack
		popad
	}
}
#pragma warning(pop)
*/

#define WRITE_P(p) { void *x = p; s.write((char*)&x, sizeof(x)); }
#define OFFSET(z) ((char*)&__context_helper.z - (char*)&__context_helper)
// need to find a better way to do calls ...
#define CALL(f) { s << '\xE8'; reloc_table.push_back(s.tellp());\
	char *x = (char*)&f; x -= 4; s.write((char*)&x, sizeof(x)); }
#define CALLP(f) { s << '\xE8'; reloc_table.push_back(s.tellp());\
	char *x = (char*)f; x -= 4; s.write((char*)&x, sizeof(x)); }
#define JMP(f) { s << '\xE9'; reloc_table.push_back(s.tellp());\
	char *x = (char*)&f; x -= 4; s.write((char*)&x, sizeof(x)); }
#define JMPP(f) { s << '\xE9'; reloc_table.push_back(s.tellp());\
	char *x = (char*)f; x -= 4; s.write((char*)&x, sizeof(x)); }
static emulation_context __context_helper; // temporary for OFFSET calculation
#define RECORD_CALLSTACK CALL

template <typename T> void write(std::ostream &s, const T &t)
{
	s.write((char*)&t, sizeof(t));
}

void compiler::load_shifter_imm()
{
	break_if_pc(ctx.rm);
	s << "\x8B\x45" << (char)OFFSET(regs[ctx.rm]); // mov eax, [ebp+rm]
	//a<MOV>( s, EAX, EBP, OFFSET(regs[ctx.rm]) );
	switch (ctx.shift)
	{
	case SHIFT::LSL:
		if (ctx.imm != 0)
			s << "\xC1\xE0" << (char)ctx.imm; // shl eax, imm
		break;
	case SHIFT::LSR:
		if (ctx.imm != 0)
			s << "\xC1\xE8" << (char)ctx.imm; // shr eax, imm
		break;
	case SHIFT::ASR:
		if (ctx.imm != 0)
			s << "\xC1\xF8" << (char)ctx.imm; // sar eax, imm
		break;
	case SHIFT::ROR:
		s << DEBUG_BREAK;
		break;
	// RRX =
	}
}


void compiler::store_flags()
{
	flags_updated = 1;
	if (lookahead_s) // next instruction would overwrite
		return;

	s << "\x0F\x90\xC0";                        // seto al
	s << '\x9F';                                // lahf
	s << "\x89\x45" << (char)OFFSET(x86_flags); // mov [ebp+x86_flags], eax

	//s << '\x9C';                                // pushfd
	//s << "\x8F\x45" << (char)OFFSET(x86_flags); // pop [ebp+x86_flags]
}

void compiler::load_flags()
{	
	s << "\x8B\x45" << (char)OFFSET(x86_flags); // mov eax, [ebp+x86_flags]
	s << "\xD0\xC8";                            // ror al, 1
	s << '\x9E';                                // sahf

	/*
	s << "\xFF\x75" << (char)OFFSET(x86_flags); // push [ebp+x86_flags]
	s << '\x9D';                                // popfd
	*/
}



void compiler::update_dest(int size)
{
	switch (ctx.addressing_mode)
	{
	case ADDRESSING_MODE::DA:
	case ADDRESSING_MODE::DB:
		s << "\x83\x6D" << (char)OFFSET(regs[ctx.rn]); // sub dword ptr[ebp+Rn],
		s << (char)(size << 2);                        // size*4
		break;
	case ADDRESSING_MODE::IA:
	case ADDRESSING_MODE::IB:
		s << "\x83\x45" << (char)OFFSET(regs[ctx.rn]); // add dword ptr[ebp+Rn],
		s << (char)(size << 2);                        // size*4
		break;
	}
}

void compiler::count(unsigned long imm, 
	unsigned long &num, 
	unsigned long &lowest, 
	unsigned long &highest,
	bool &region)
{
	int i;
	num = 0;
	for (i = 0; i < 16; i++)
	{
		unsigned long b = imm & 1;
		imm >>= 1;
		if (b)
		{
			num++;
			lowest = i;
			highest = i;
			break;
		}
	}
	i++;
	for (;i < 16; i++)
	{
		unsigned long b = imm & 1;
		imm >>= 1;
		if (b)
		{
			num++;
			highest = i;
		}
	}
	region = (highest - lowest + 1) == num;
}

// loads ecx with the destination address if there is only
// one register to be stored/loaded
void compiler::load_ecx_single()
{
	s << "\x8B\x4D" << (char)OFFSET(regs[ctx.rn]);  // mov ecx, dword ptr [ebp+Rn] 
	switch (ctx.addressing_mode)
	{
	case ADDRESSING_MODE::DA: break; // start at Rn - 1*4 + 4 = Rn
	case ADDRESSING_MODE::IA: break; // start at Rn
	case ADDRESSING_MODE::DB:        // start at Rn - 1*4
		s << "\x83\xE9\x04"; // sub ecx, 4
		break;
	case ADDRESSING_MODE::IB:        // start at Rn+4
		s << "\x83\xC1\x04"; // add ecx, 4
		break;
	}
}

// pushs the start address if multiple regs are to be stored/loaded
void compiler::push_multiple(int num)
{
	switch (ctx.addressing_mode)
	{
	case ADDRESSING_MODE::IA: // start at Rn
		s << "\xFF\x75" << (char)OFFSET(regs[ctx.rn]); // push dword ptr[ebp+Rn]
		break;
	case ADDRESSING_MODE::IB: // start at Rn+4
		s << "\x8B\x4D" << (char)OFFSET(regs[ctx.rn]); // mov ecx, dword ptr[ebp+Rn]
		s << "\x83\xC1\x04";                           // add ecx, 4
		s << "\x51";                                   // push ecx
		break;
	case ADDRESSING_MODE::DA: // start at Rn - num*4 + 4, num >= 1
		s << "\x8B\x4D" << (char)OFFSET(regs[ctx.rn]); // mov ecx, dword ptr[ebp+Rn]
		s << "\x83\xE9" << (char)((num-1) << 2);       // sub ecx, (num-1)*4
		s << "\x51";                                   // push ecx
		break;
	case ADDRESSING_MODE::DB: // start at Rn - num * 4, mum >= 1
		s << "\x8B\x4D" << (char)OFFSET(regs[ctx.rn]); // mov ecx, dword ptr[ebp+Rn]
		s << "\x83\xE9" << (char)(num << 2);           // sub ecx, num*4
		s << "\x51";                                   // push ecx
		break;
	}
}

// loads a register content or PC relative address to ecx
void compiler::load_ecx_reg_or_pc(int reg, unsigned long offset)
{
	if (reg != 0xF)
	{
		s << "\x8B\x4D" << (char)OFFSET(regs[reg]);      // mov ecx, [ebp+rn]
	} else
	{
		// ip relative
		s << "\x8B\x4D" << (char)OFFSET(regs[ctx.rn]);    // mov ecx, [ebp+rn]
		s << "\x81\xE1"; write( s, (unsigned long)(~PAGING::ADDRESS_MASK) ); // and ecx, ~PAGING::ADDR_MASK
		offset += ((inst+2) << INST_BITS) & ~3;
	}
	add_ecx(offset);
	
}


void compiler::break_if_pc(int reg)
{
	if (reg == 0xF)
		s << DEBUG_BREAK;
}

void compiler::break_if_thumb()
{
	if (INST_BITS == 1)
		s << DEBUG_BREAK;
}

void compiler::load_edx_ecx_or_reg(int r1, int r2)
{
	if (r1 == r2)                                   // reuse same reg?
		s << "\x8B\xD1";                            // mov edx, ecx
	else s << "\x8B\x55" << (char)OFFSET(regs[r1]); // mov edx, [ebp+rn]
}

void compiler::load_eax_ecx_or_reg(int r1, int r2)
{
	if (r1 == r2)                                   // reuse same reg?
		s << "\x8B\xC1";                            // mov eax, ecx
	else s << "\x8B\x45" << (char)OFFSET(regs[r1]); // mov eax, [ebp+rn]
}

void compiler::shiftop_eax_ecx()
{
	// TODO add code to handle ecx > 255 (31)
	switch (ctx.shift)
	{
	case SHIFT::LSL:
		s << "\xD3\xE0"; // shl eax, cl
		break;
	case SHIFT::LSR:
		s << "\xD3\xE8"; // shr eax, cl
		break;
	case SHIFT::ASR:
		s << "\xD3\xF8"; // sar eax, cl
		break;
	case SHIFT::ROR:
		s << "\xD3\xC8"; // ror eax, cl
		break;
	}
}


void compiler::add_ecx(unsigned long imm)
{
	if (imm)
	{
		s << "\x81\xC1"; write( s, imm ); // add ecx, imm
	}
}

void compiler::generic_store()
{
	break_if_pc(ctx.rd);                 // todo handle rd = PC
	load_ecx_reg_or_pc(ctx.rn);          // ecx = reg[rn]
	load_edx_ecx_or_reg(ctx.rd, ctx.rn); // edx = reg[rd]
}

void compiler::generic_loadstore_postupdate_imm()
{
	if (ctx.imm)
	{
		s << "\x81\x45" << (char)OFFSET(regs[ctx.rn]); // add [ebp+Rn],
		write( s, ctx.imm );                           //  imm
	}
}

void compiler::generic_loadstore_postupdate_ecx()
{
	s << "\x89\x4D" << (char)OFFSET(regs[ctx.rn]); // mov [ebp+rn], ecx
}


void compiler::generic_store_p()
{
	generic_store();
	add_ecx(ctx.imm);                    // ecx += imm
}


void compiler::generic_store_r()
{
	generic_store();
	generic_loadstore_shift();
}

void compiler::generic_loadstore_shift()
{
	if ((ctx.shift == SHIFT::LSL) && (ctx.imm == 0))
	{
		// simply add Rm to ecx
		s << "\x03\x4D" << (char)OFFSET(regs[ctx.rm]); // add ecx, [ebp+rm]
	} else
	{
		s << DEBUG_BREAK;
		// shifters not supported yet
	}
}

void compiler::generic_load()
{
	break_if_pc(ctx.rd);                  // todo handle rd = PC
	load_ecx_reg_or_pc(ctx.rn, ctx.imm);  // ecx = reg[rn] + imm
}

void compiler::generic_load_post()
{
	break_if_pc(ctx.rd);            // todo handle rd = PC
	load_ecx_reg_or_pc(ctx.rn, 0);  // ecx = reg[rn]
}

void compiler::generic_load_r()
{
	generic_load();
	generic_loadstore_shift();
}

void compiler::generic_load_x()
{
	switch (ctx.extend_mode)
	{
	case EXTEND_MODE::H:  CALL(HLE<_ARM9>::load16u); break;
	//case EXTEND_MODE::SB: CALL(HLE<_ARM9>::load8s); break; // special instruction ...
	case EXTEND_MODE::SH: CALL(HLE<_ARM9>::load16s); break; // special instruction ...
	default:
		s << DEBUG_BREAK;
	}
}

void compiler::generic_store_x()
{
	switch (ctx.extend_mode)
	{
	case EXTEND_MODE::H:  CALL(HLE<_ARM9>::store16); break;
	//case EXTEND_MODE::SB: CALL(HLE<_ARM9>::store8s); break; // special instruction ...
	//case EXTEND_MODE::SH: CALL(HLE<_ARM9>::store16s); break; // special instruction ...
	default:
		s << DEBUG_BREAK;
	}
}


const unsigned long cpsr_masks[16] =
{
	0x00000000,
	0x000000FF,
	0x0000FF00,
	0x0000FFFF,
	0x00FF0000,
	0x00FF00FF,
	0x00FFFF00,
	0x00FFFFFF,
	0xFF000000,
	0xFF0000FF,
	0xFF00FF00,
	0xFF00FFFF,
	0xFFFF0000,
	0xFFFF00FF,
	0xFFFFFF00,
	0xFFFFFFFF
};

// EVERYTHING HERE IS ONLY VALID FOR THE ARM9 SO FAR!!!

compiler::compiler() : s(std::ostringstream::binary | std::ostringstream::out)
{
	flags_updated = false;
	preoff = 0;
}

void compiler::record_callstack()
{
	s << '\x51'; // push ecx
	CALL(HLE<_ARM9>::pushcallstack);
	s << '\x59'; // pop ecx
}

void compiler::update_callstack()
{
	s << '\x51'; // push ecx
	CALL(HLE<_ARM9>::popcallstack);
	s << '\x59'; // pop ecx
}

void compiler::compile_instruction()
{
	bool patch_jump = false;
	std::ostringstream::pos_type jmpbyte;
	unsigned long bpre = preoff;
	preoff = 0;
	int flags_actual = flags_updated;

	if (ctx.cond != CONDITION::AL)
	{
		if (ctx.cond != CONDITION::NV)
		{
			if (!flags_actual)
			{
				load_flags();
				flags_actual = 1;
			}
			switch (ctx.cond)
			{
			// conditions are _reversed_ in the jumps because a jump taken
			// means the instruction will _not_ be executed.
			case CONDITION::EQ: s << '\x75'; break; // jnz  (ZF = 0)
			case CONDITION::NE: s << '\x74'; break; // jz   (ZF = 0)
			case CONDITION::CS: s << '\x72'; break; // jc   (CF = 1) 
			case CONDITION::CC: s << '\x73'; break; // jnc  (CF = 0)
			case CONDITION::MI: s << '\x79'; break; // jns  (SF = 0)
			case CONDITION::PL: s << '\x78'; break; // js   (SF = 1)
			case CONDITION::VS: s << '\x71'; break; // jno  (OF = 0)
			case CONDITION::VC: s << '\x70'; break; // jo   (OF = 1)
			case CONDITION::HI: s << '\x76'; break; // jbe  (CF = 1 or  ZF = 1)
			case CONDITION::LS: s << '\x77'; break; // jnbe (CF = 0 and ZF = 0)
			case CONDITION::GE: s << '\x7C'; break; // jl   (SF != OF)
			case CONDITION::LT: s << '\x7D'; break; // jge  (SF == OF)
			case CONDITION::GT: s << '\x7E'; break; // jle  (ZF = 0 or  SF != OF)
			case CONDITION::LE: s << '\x7F'; break; // jg   (ZF = 0 and SF == OF)
			}
			jmpbyte = s.tellp();
			patch_jump = true;
			s << '\x00';
		}
	}
	flags_updated = 0;

////////////////////////////////////////////////////////////////////////////////
	switch (ctx.instruction)
	{
	case INST::MOV_I:
		break_if_pc(ctx.rd);
		s << "\xC7\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], imm32
		write( s, ctx.imm );
		if (ctx.flags & disassembler::S_BIT)
		{
			s << "\x83\x4D" << (char)OFFSET(regs[ctx.rd]) << '\0'; // or [ebp+rd], 0
			store_flags();
		}
		break;

	case INST::MOV_R:
		load_shifter_imm();
		

		if (ctx.rd == 0xF)
		{
			if (ctx.flags & disassembler::S_BIT)
				s << DEBUG_BREAK;
			s << "\x8B\x4D" << (char)OFFSET(regs[15]); // mov ecx, [r15]
			s << "\x83\xE1\x01";                       // and ecx, 1
			s << "\x0B\xC8";                           // or ecx, eax
			s << "\x89\x4D" << (char)OFFSET(regs[15]); // mov [ebp+rd], ecx
			JMP(HLE<_ARM9>::compile_and_link_branch_a)
		} else
		{
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}

		if (ctx.flags & disassembler::S_BIT)
		{
			s << "\x83\xC8" << '\x0'; // or eax, 0
			store_flags();
		}
		break;

	case INST::MOV_RR: 	// "MOV%c%s %Rd,%Rm,%S %Rs",
		break_if_pc(ctx.rd);
		load_ecx_reg_or_pc(ctx.rs);          // ecx = reg[rn]
		load_eax_ecx_or_reg(ctx.rm, ctx.rs); // edx = reg[rd]
		shiftop_eax_ecx();
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		if (ctx.flags & disassembler::S_BIT)
		{
			// todo optimize the flagcheck (doesnt need to do the cmp)
			s << "\x83\xC8" << '\x0'; // or eax, 0
			store_flags();
		}
		break;

	case INST::MVN_I:
		break_if_pc(ctx.rd);
		s << "\xC7\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], ~imm32
		write( s, ~ctx.imm );
		break;
	case INST::MVN_R:
		break_if_pc(ctx.rd);
		if ((ctx.rd == ctx.rm) && (ctx.shift == SHIFT::LSL) && (ctx.imm == 0))
		{
			s << "\xF7\x55" << (char)OFFSET(regs[ctx.rd]);  // not [ebp+rd]
		} else
		{
			load_shifter_imm();
			s << "\xF7\xD0";                                // not eax
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;
	case INST::STR_I:
		generic_store();
		CALL(HLE<_ARM9>::store32)
		generic_loadstore_postupdate_imm();		
		break;

	case INST::STRX_IP:
		generic_store_p();
		generic_store_x();
		break;
	case INST::STRX_RP:
		generic_store_r();
		generic_store_x();
		break;
	case INST::STRB_IP:
		generic_store_p();
		CALL(HLE<_ARM9>::store8) 
		break;
	case INST::STR_IP:
		generic_store_p();
		CALL(HLE<_ARM9>::store32) 
		break;
	case INST::STR_RP:
		generic_store_r();
		CALL(HLE<_ARM9>::store32) 
		break;
	case INST::STRB_RP:
		generic_store_r();
		CALL(HLE<_ARM9>::store8) 
		break;


	// Post index loads
	case INST::LDR_I:
		generic_load_post();
		generic_loadstore_postupdate_imm();
		CALL(HLE<_ARM9>::load32)
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		break;

	// Pre index loads
	case INST::LDR_IP:
		generic_load();
		CALL(HLE<_ARM9>::load32)
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		break;
	case INST::LDR_RP:
		generic_load_r();
		CALL(HLE<_ARM9>::load32) 
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		break;
	case INST::LDRB_IP: 
		generic_load();
		CALL(HLE<_ARM9>::load8u)
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		break;
	case INST::LDRB_RP: 
		generic_load_r();
		CALL(HLE<_ARM9>::load8u)
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		break;
	case INST::LDRX_IP:
		generic_load();
		generic_load_x();
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		break;
	case INST::LDRX_RP:
		generic_load_r();
		generic_load_x();
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);  // mov [ebp+rd], eax
		break;


	case INST::BLX_I:
		// Branch and link
		s << "\x8B\x4D" << (char)OFFSET(regs[15]);             // mov ecx, [ebp+R15]
		s << "\x81\xE1"; write( s, (unsigned long)(~PAGING::ADDRESS_MASK | 1) ); // and ecx, ~PAGING::ADDR_MASK | 1
		s << "\x81\xC1"; write( s, (unsigned long)(inst+1) << INST_BITS);        // add ecx, imm
		s << "\x89\x4D" << (char)OFFSET(regs[14]);             // mov [ebp+r14], ecx
		record_callstack();
		s << "\x81\xE1"; write( s, (unsigned long)(~1) );  // and ecx, ~1
		s << "\x83\xE0\xFE";                               // and eax, 0FFFFFFFEh 
		s << "\x81\xC1"; write( s, bpre + ctx.imm - 4 );   // add ecx, imm
		s << "\x89\x4D" << (char)OFFSET(regs[15]);         // mov [ebp+r15], ecx
		JMP(HLE<_ARM9>::compile_and_link_branch_a)
		break;
	case INST::BL:
		// Branch and link
		s << "\x8B\x4D" << (char)OFFSET(regs[15]);             // mov ecx, [ebp+R15]
		s << "\x81\xE1"; write( s, (unsigned long)(~PAGING::ADDRESS_MASK | 1) ); // and ecx, ~PAGING::ADDR_MASK | 1
		s << "\x81\xC1"; write( s, (unsigned long)(inst+1) << INST_BITS);        // add ecx, imm
		s << "\x89\x4D" << (char)OFFSET(regs[14]);             // mov [ebp+r14], ecx
		record_callstack();
		s << "\x81\xC1"; write( s, bpre + ctx.imm - 4 );       // add ecx, imm
		s << "\x89\x4D" << (char)OFFSET(regs[15]);             // mov [ebp+r15], ecx
		JMP(HLE<_ARM9>::compile_and_link_branch_a)
		break;
	case INST::BX:
		// Branch to register (generally R14)
		s << "\x8B\x4D" << (char)OFFSET(regs[ctx.rm]); // mov ecx, [ebp+rm]
		s << "\x89\x4D" << (char)OFFSET(regs[15]);     // mov [ebp+r15], ecx
		update_callstack();
		JMP(HLE<_ARM9>::compile_and_link_branch_a)
		//s << "\xFF\xE0";                               // jmp eax
		break;

	case INST::BLX: // Branch and link register
		// link
		s << "\x8B\x4D" << (char)OFFSET(regs[15]);             // mov ecx, [ebp+R15]
		s << "\x81\xE1"; write( s, (unsigned long)(~PAGING::ADDRESS_MASK | 1) ); // and ecx, ~PAGING::ADDR_MASK | 1
		s << "\x81\xC1"; write( s, (unsigned long)(inst+1) << INST_BITS); // add ecx, imm
		s << "\x89\x4D" << (char)OFFSET(regs[14]);             // mov [ebp+r14], ecx
		record_callstack();
		// branch
		s << "\x8B\x4D" << (char)OFFSET(regs[ctx.rm]);         // mov ecx, [ebp+Rm]
		s << "\x89\x4D" << (char)OFFSET(regs[15]);             // mov [ebp+r15], ecx
		JMP(HLE<_ARM9>::compile_and_link_branch_a)
		//s << "\xFF\xE0";                                       // jmp eax
		break;
	// case BLX_I => use +bpre

	case INST::B:
		// This branch jumps correct now
		s << "\x8B\x4D" << (char)OFFSET(regs[15]);    // mov ecx, [ebp+R15]
		s << "\x81\xE1"; write( s, (unsigned long)(~PAGING::ADDRESS_MASK | 1) ); // and ecx, ~PAGING::ADDR_MASK | 1
		s << "\x81\xC1"; write( s, ctx.imm + (unsigned long)((inst) << INST_BITS)); // add ecx, imm
		s << "\x89\x4D" << (char)OFFSET(regs[15]);    // mov [ebp+r15], ecx
		update_callstack();
		JMP(HLE<_ARM9>::compile_and_link_branch_a)
		//s << "\xFF\xE0";                              // jmp eax
		break;
	case INST::BPRE:
		preoff = ctx.imm;
		s << "\x90";
		break;



	// MCR => ARM -> Coprocessor (write to Coproc)
	// MRC => ARM <- Coprocessor (read from Coproc)

	// this is pretty much like machine specific registers on x86
	case INST::MRC: // transfer to registers
		switch (ctx.cp_num) 
		{
		case 0xF: // System control coprocessor
			if (ctx.cp_op1 != 0) // SBZ
				goto default_;   // unpredictable
			if (ctx.rd == 0xF)   // Rd must not be R15
				goto default_;   // unpredictable
			switch (ctx.rn) // select register
			{
			case 0: // ID codes (read only)
				switch (ctx.cp_op2)
				{
				//case 1:  // Cache Type register (ensata doesnt emulate this, no$gba crashs lol) 
				//	break; // spec says only 0 is mandatory so this is ok
				case 0: // Main ID register
				default: // if unimplemented register is queried return Main ID register
					// no$gba returns 0x41059461
					s << "\xC7\x45" << (char)OFFSET(regs[ctx.rd]); write( s, (unsigned long)0x41059460 );
					break;
				}
				break;
			case 1: // control bits (r/w)
				// SBZ but ignored
				//if ((ctx.rm != 0) || (ctx.cp_op2 != 0))
				//	goto default_;
				s << "\x8B\x45" << (char)OFFSET(syscontrol.control_register); // mov eax,[ebp+creg]
				s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]);                // mov [ebp+rd],eax
				break;
			default:
				goto default_; // not supported yet
			}
			break;
		default:
			goto default_;
		}
		break;


	case INST::MCR: // load coproc from arm register
		switch (ctx.cp_num) 
		{
		case 0xF: // System control coprocessor
			if (ctx.cp_op1 != 0) // SBZ
				goto default_;   // unpredictable
			if (ctx.rd == 0xF)   // Rd must not be R15
				goto default_;   // unpredictable
			switch (ctx.rn) // select register
			{
			
			case 1: // control bits (r/w)
				// SBZ but ignored
				//if ((ctx.rm != 0) || (ctx.cp_op2 != 0))
				//	goto default_;
				s << "\x8B\x45" << (char)OFFSET(regs[ctx.rd]);                // mov eax, [ebp+rd]
				s << "\x89\x45" << (char)OFFSET(syscontrol.control_register); // mov [ebp+creg], eax
				s << "\x0B\xC0"; // or eax, eax
				s << "\x75\x05"; // jmp $+5
				s << '\xB8'; write(s, (unsigned long)syscontrol_context::CONTROL_DEFAULT );
				// mov eax, syscontrol_context::CONTROL_DEFAULT
				break;
			
			// MMU/PU not supported yet
			case 2:
			case 3:
			case 5:
			case 6: // CRm and opcode 2 select the protected region/bank
			 
				s << NOT_SUPPORTED_YET_BUT_SKIP;

			case 7: // cache and write buffers (write only)
				s << '\x90'; // not yet emulated or unimportant xD
				break;
			case 9: // cache lockdown or TCM remapping
				switch (ctx.rm)
				{
				case 0x1: // TCM remapping
					load_ecx_reg_or_pc(ctx.rd);           // mov ecx, [ebp+rd]
					s << '\xBA'; s.write( (char*)&ctx.cp_op2, sizeof(ctx.cp_op2) ); // mov edx, ctx.cp_op2
					CALL(HLE<_ARM9>::remap_tcm);
					break;
				default:
					s << '\x90'; // lockdown not emulated
					break;
				}
				break;

			case 0: // may not write to this register
			default:
				goto default_; // not supported yet
			}
			break;
		default:
			goto default_;
		}
		break;

	case INST::BIC_R: // Rd = Rn & ~shifter_imm
		load_shifter_imm();
		s << "\xF7\xD0";                               // not eax;
		if (ctx.rn == ctx.rd)
		{
			s << "\x21\x45" << (char)OFFSET(regs[ctx.rn]); // and [ebp+rd], eax
		} else
		{
			s << "\x23\x45" << (char)OFFSET(regs[ctx.rn]); // and eax, [ebp+rn]
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags(); // todo: set carry = shifter carry
		break;
	case INST::AND_R: // Rd = Rn & shifter_imm
		load_shifter_imm();
		if (ctx.rn == ctx.rd)
		{
			s << "\x21\x45" << (char)OFFSET(regs[ctx.rn]); // and [ebp+rd], eax
		} else
		{
			s << "\x23\x45" << (char)OFFSET(regs[ctx.rn]); // and eax, [ebp+rn]
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags(); // todo: set carry = shifter carry
		break;
	case INST::EOR_R: // Rd = Rn ^ shifter_imm
		load_shifter_imm();
		if (ctx.rn == ctx.rd)
		{
			s << "\x31\x45" << (char)OFFSET(regs[ctx.rn]); // xor [ebp+rd], eax
		} else
		{
			s << "\x33\x45" << (char)OFFSET(regs[ctx.rn]); // xor eax, [ebp+rn]
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags(); // todo: set carry = shifter carry
		break;


	case INST::BIC_I:
		s << "\x8B\x45" << (char)OFFSET(regs[ctx.rn]); // mov eax,[ebp+Rn]
		s << '\x25'; write(s, ~ctx.imm);               // and eax, ~imm
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;

	case INST::ORR_I: // Rd = Rn | imm
		if (ctx.rn == ctx.rd)
		{
			s << "\x81\x4D" << (char)OFFSET(regs[ctx.rn]); // or [ebp+rn],
			write( s, ctx.imm );                           //  imm
		} else
		{
			s << "\x8B\x45" << (char)OFFSET(regs[ctx.rn]); // mov eax, [ebp+rn]
			s << "\x0D"; write(s, ctx.imm );               // or eax, imm
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;

	case INST::AND_I: // Rd = Rn & imm
		if (ctx.rn == ctx.rd)
		{
			s << "\x81\x65" << (char)OFFSET(regs[ctx.rn]); // and [ebp+rn],
			write( s, ctx.imm );                           //  imm
		} else
		{
			s << "\x8B\x45" << (char)OFFSET(regs[ctx.rn]); // mov eax, [ebp+rn]
			s << "\x25"; write(s, ctx.imm );               // and eax, imm
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;

	case INST::ADD_I: // Rd = Rn + imm
		if (ctx.rn == ctx.rd)
		{
			s << "\x81\x45" << (char)OFFSET(regs[ctx.rn]); // add [ebp+rn],
			write( s, ctx.imm );                           // imm
		} else
		{
			s << "\x8B\x45" << (char)OFFSET(regs[ctx.rn]); // mov eax, [ebp+rn]
			// TODO: optimize (can be ommited if imm == 0 and lookahead_s is set)
			// can be inc/dec when imm = 0 or FFFFFFFF
			s << "\x05"; write(s, ctx.imm );               // add eax, imm
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;
	case INST::ADD_R: // Rd = Rn + shifter_imm
		{
			load_shifter_imm();
			if (ctx.rn == ctx.rd)
			{
				s << "\x01\x45" << (char)OFFSET(regs[ctx.rn]); // add [ebp+Rn], eax
			} else
			{
				s << "\x03\x45" << (char)OFFSET(regs[ctx.rn]); // add eax, [ebp+Rn]
				s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
			}
			if (ctx.flags & disassembler::S_BIT)
				store_flags();
			break;
		}
	case INST::ADC_R: // Rd = Rn + shifter_imm + carry
		{
			load_shifter_imm();
			if (!flags_actual)
				load_flags();
			if (ctx.rn == ctx.rd)
			{
				s << "\x11\x45" << (char)OFFSET(regs[ctx.rn]); // adc [ebp+Rn], eax
			} else
			{
				s << "\x13\x45" << (char)OFFSET(regs[ctx.rn]); // adc eax, [ebp+Rn]
				s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
			}



			if (ctx.flags & disassembler::S_BIT)
				store_flags();
			break;
		}

	case INST::SUB_I: // Rd = Rn - imm
		if (ctx.rn == ctx.rd)
		{
			s << "\x81\x6D" << (char)OFFSET(regs[ctx.rn]); // sub [ebp+rn],
			write( s, ctx.imm );                           // imm
		} else
		{
			s << "\x8B\x45" << (char)OFFSET(regs[ctx.rn]); // mov eax, [ebp+rn]
			s << "\x2D"; write(s, ctx.imm );               // sub eax, imm
			s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		}
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;
	case INST::SUB_R: // Rd = Rn - shifter_imm
		{
			load_shifter_imm();
			if (ctx.rn == ctx.rd)
			{
				s << "\x29\x45" << (char)OFFSET(regs[ctx.rn]); // sub [ebp+Rn], eax
			} else
			{
				s << "\x8B\x55" << (char)OFFSET(regs[ctx.rn]); // mov edx, [ebp+Rn]
				s << "\x2B\xD0";                               // sub edx, eax
				s << "\x89\x55" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], edx
			}
			if (ctx.flags & disassembler::S_BIT)
				store_flags(); // todo: set carry = shifter carry
			break;
		}


	case INST::SBC_R: // Rd = Rn - shifter_imm - carry
		{
			load_shifter_imm();
			if (!flags_actual)
				load_flags();
			if (ctx.rn == ctx.rd)
			{
				s << "\x19\x45" << (char)OFFSET(regs[ctx.rn]); // sbb [ebp+Rn], eax
			} else
			{
				s << "\x8B\x55" << (char)OFFSET(regs[ctx.rn]); // mov edx, [ebp+Rn]
				s << "\x1B\xD0";                               // sbb edx, eax
				s << "\x89\x55" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], edx
			}
			if (ctx.flags & disassembler::S_BIT)
				store_flags(); // todo: set carry = shifter carry
			break;
		}

	case INST::RSB_I:
		if (ctx.rn == ctx.rd)
		{
			if (ctx.imm == 0)
			{
				s << "\xF7\x5D" << (char)OFFSET(regs[ctx.rn]); // neg [ebp+Rn]
				if (ctx.flags & disassembler::S_BIT)
					store_flags();
				break;
			}
		} else
		{
			if (ctx.imm == 0)
			{
				// dst = neg src
				s << "\x8B\x45" << (char)OFFSET(regs[ctx.rn]); // mov eax, [ebp+rn]
				s << "\xF7\xD8";                               // neg eax
				s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
				if (ctx.flags & disassembler::S_BIT)
					store_flags();				
				break;
			}
		}
		s << '\x90' << DEBUG_BREAK;
		break;

	case INST::ORR_R: // Rd = Rn | shifter_imm
		load_shifter_imm();
		s << "\x0B\x45" << (char)OFFSET(regs[ctx.rn]); // or eax, [ebp+Rn]
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;
	case INST::CLZ:
		
		s << DEBUG_BREAK;
		s << "\x8B\x45" << (char)OFFSET(regs[ctx.rm]); // mov eax, [ebp+Rm]
		s << "\x33\xC9"; // xor ecx, ecx
		s << "\xD1\xE8"; // shr eax, 1
		s << "\x74\x01"; // jz $+1
		s << "\x41";     // inc ecx
		s << "\xB8"; write(s, (unsigned long)0x20); // mov eax, 20
		s << "\x2B\xC1"; // sub eax, ecx
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		break;


	case INST::MUL_R: // Rd = Rm * Rs (Rd must not be Rm)
		s << "\x33\xD2"; // xor edx, edx
		s << "\x8B\x45" << (char)OFFSET(regs[ctx.rm]); // mov eax, [ebp+rd]
		s << "\xF7\x65" << (char)OFFSET(regs[ctx.rs]); // mul [ebp+rs]
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		if (ctx.flags & disassembler::S_BIT)
			store_flags();
		break;

	case INST::MRS_CPSR:
		s << "\x8B\x45" << (char)OFFSET(cpsr); // mov eax, [ebp+cpsr]
		s << "\x89\x45" << (char)OFFSET(regs[ctx.rd]); // mov [ebp+rd], eax
		break;

	case INST::MSR_CPSR_R:
		// mask = Rn

		// todo: priviledge mode check depending on mask

		if (ctx.rn == 0)
			s << "\x90"; // nothing masked = nothing to write...
		else
		{
			if (ctx.rn & 0x7)
				CALL(HLE<_ARM9>::is_priviledged)
			
			s << "\x8B\x45" << (char)OFFSET(regs[ctx.rm]); // mov eax, [ebp+Rm]
			if (ctx.rn != 0xF)
			{
				// mask out
				s << '\x25'; write(s, cpsr_masks[ctx.rn]); // and eax, mask
			}
			// store to cpsr
			s << "\x89\x45" << (char)OFFSET(cpsr);
		}
		break;
	case INST::TST_I: // TST Rn, imm
		s << "\xF7\x45" << (char)OFFSET(regs[ctx.rn]); // test [ebp+Rm],
		write( s, ctx.imm );                           // imm
		store_flags();
		break;
	case INST::TST_R:
		break_if_pc(ctx.rn);
		load_shifter_imm(); // eax = [ebp+Rm] x shifter
		s << "\x85\x45" << (char)OFFSET(regs[ctx.rn]); // test [ebp+Rd], eax
		store_flags();		
		break;

	case INST::CMP_I:
		break_if_pc(ctx.rn);
		s << "\x81\x7D" << (char)OFFSET(regs[ctx.rn]); // cmp [ebp+Rn],
		write(s, ctx.imm);                             // imm
		store_flags();
		break;
	case INST::CMP_R:
		break_if_pc(ctx.rn);
		load_shifter_imm();
		s << "\x39\x45" << (char)OFFSET(regs[ctx.rn]); // cmp [ebp+Rn], eax
		store_flags();
		break;

	case INST::STM_W:
		{
			unsigned long num, highest, lowest;
			bool region;
			count( ctx.imm, num, lowest, highest, region );
			switch (num)
			{
			case 0: // spec says this is undefined
				s << DEBUG_BREAK;
				break;
			case 1: // simple 32bit store
				load_ecx_single();      // load ecx, start_address
				s << "\x8B\x55" << (char)OFFSET(regs[highest]); // mov edx, dword ptr [ebp+R_highest]
				CALL(HLE<_ARM9>::store32) // dont use array store but simple store here!
				break;
			default:
				unsigned int pop = 0;
				if (region) // optimized version when a region Ra-Rb is stored
				{
					char ofs = (char)OFFSET(regs[lowest]);
					if (ofs == 0)
						s << '\x55';            // push ebp
					else
					{
						s << "\x8D\x45" << ofs; // lea eax, [ebp+Rlow]
						s << '\x50';                // push eax
					}
				} else
				{
					pop = num;
					for (int i = 15; i >= 0; i--)
					{
						if (ctx.imm & (1 << i))
						{
							s << "\xFF\x75" << (char)OFFSET(regs[i]); // push dword ptr[ebp+Ri]
						}
					}

					s << '\x54';              // push esp
				}
				s << '\x6A' << (char)num; // push num
				
				// determine start address
				push_multiple(num);
				CALL(HLE<_ARM9>::store32_array) 
				s << "\x83\xC4" << (char)((pop+3) << 2);       // add esp, num*4
			}

			// w-bit is set, update destination register
			// but only when it was not in the loaded list!
			if (!(ctx.imm & (1 << ctx.rn)))
				update_dest(num);
		}
		break;

	case INST::LDM_W:
		{
			unsigned long num, highest, lowest;
			bool region;
			count( ctx.imm, num, lowest, highest, region );
			switch (num)
			{
			case 0: // spec says this is undefined
				s << DEBUG_BREAK;
				break;
			case 1: // simple 32bit store
				load_ecx_single();      // load ecx, start_address
				CALL(HLE<_ARM9>::load32) // dont use array load but simple load here!
				s << "\x89\x45" << (char)OFFSET(regs[highest]); // mov [ebp+R_highest], eax
				break;
			default:
				if (region) // optimized version when a region Ra-Rb is stored
				{
					char ofs = (char)OFFSET(regs[lowest]);
					if (ofs == 0)
						s << '\x55';            // push ebp
					else
					{
						s << "\x8D\x45" << ofs; // lea eax, [ebp+Rlow]
						s << '\x50';            // push eax
					}
					s << '\x6A' << (char)num;   // push num
					push_multiple(num);
					CALL(HLE<_ARM9>::load32_array) 
					s << "\x83\xC4\x0C";        // add esp, 3*4
				} else
				{
					//s << '\x90' << DEBUG_BREAK; // not supported yet
					s << "\x83\xEC" << (char)(num << 2); // sub esp, num*4
					s << '\x54';                         // push esp
					s << '\x6A' << (char)num;            // push num
					push_multiple(num);
					CALL(HLE<_ARM9>::load32_array) 
					s << "\x83\xC4\x0C";                 // add esp, 3*4

										
					unsigned long mask = ctx.imm;
					for (int i = 0; i < 16; i++)
					{
						if (mask & 1)
						{
							s << "\x8F\x45" << (char)OFFSET(regs[i]); // pop dword ptr[ebp+Ri]
						}
						mask >>= 1;
					}
				}
				break;
			}
			// w-bit is set, update destination register
			update_dest(num);

			// if PC was specified handle the jump!
			if (ctx.imm & (1 << 15))
			{
				s << "\x8B\x4D" << (char)OFFSET(regs[15]); // mov ecx, [ebp+R15]
				update_callstack();
				JMP(HLE<_ARM9>::compile_and_link_branch_a)
			}
		}
		break;

	case INST::SWI:
		s << "\xB9"; write(s, ctx.imm); // mov ecx, imm
		CALL(HLE<_ARM9>::swi);
		break;

	case INST::UD:
	default:
	default_:
		s << DEBUG_BREAK; // no idea how to handle this!
	}

	if (patch_jump)
	{
		std::ostringstream::pos_type cur = s.tellp();
		size_t off = cur - jmpbyte - 1;
		if(off >= 128)
		{
			char buffer[100];
			sprintf_s(buffer, 100, "Generated code too large: %i", off );
			MessageBoxA(0,buffer,0,0);
			assert(0);
		}
		s.seekp( jmpbyte );
		s << (char)off;
		s.seekp( cur );
		patch_jump = false;
	}
}

void compiler::epilogue(char *&mem, size_t &size)
{
	//s << DEBUG_BREAK << '\xC3'; // terminate with int 3 and return

	// branch to next page
	s << "\x8B\x4D" << (char)OFFSET(regs[15]); // mov ecx, [ebp+R15]
	s << "\x81\xE1"; write( s, (unsigned long)(~PAGING::ADDRESS_MASK | 1) ); // and ecx, ~PAGING::ADDR_MASK | 1
	s << "\x81\xC1"; write( s, PAGING::SIZE);  // add ecx, imm
	s << "\x89\x4D" << (char)OFFSET(regs[15]); // mov [ebp+r15], ecx
	JMP(HLE<_ARM9>::compile_and_link_branch_a)
	//s << "\xFF\xE0";                           // jmp eax
	
	// some instruction reaches end of block
	// this has to be replaced with a jump to the next block

	size = s.tellp();
	mem = new char[size];
	mprotect( mem, size, PROT_READ | PROT_WRITE | PROT_EXEC );

#pragma warning(push)
#pragma warning(disable: 4996)
	s.str().copy( mem, size );
#pragma warning(push)


	// relocate all relative addresses
	for (std::list<unsigned long>::iterator it = reloc_table.begin(); 
		it != reloc_table.end(); ++it)
	{
		char** c = (char**)&mem[*it];
		*c -= (size_t)c;
	}
}
#undef OFFSET

std::ostringstream::pos_type compiler::tellp()
{
	return s.tellp();
}