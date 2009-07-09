#ifndef _RUNNER_H_
#define _RUNNER_H_

#include "BreakpointBase.h"
#include "Breakpoint.h"

#ifndef WIN32
#define UlongToPtr(l) (void*)l
#endif

bool check_mem_access(void *start, size_t sz);

template <typename T> class breakpoints_base;
template <typename T, typename U> class breakpoints;

template <typename T>
struct exception_context
{
	static host_context context;
};
template <typename T> host_context exception_context<T>::context;

// This definitly needs some more reliable way
// i.) currently having issues when R15 is out of date
//     (crash within R15 writing instruction after write but before jump)
// ii.) ebp unwinding will only work in debug mode when stack frames are 
//      enabled. Release build omits them for performance
//
// A possible (not really nice solution either though) would be using
// TF for single stepping out of the HLE till reaching either the JIT
// block R15 points at or till reaching the run/continuation call


template <typename T, typename U>
bool resolve_armpc_from_eip2(unsigned long R15, void *eip)
{
	memory_block *fault_page = memory_map<_ARM9>::addr2page(R15);
	compiled_block<U> *block = fault_page->get_jit<T, U>();

	// is eip within the block?
	if ((eip < block->code) || eip >= block->code + block->code_size)
		return false;

	// find the JIT instruction of the crash
	int i = 0;
	while ((eip >= block->remap[i+1]) && (i < block->REMAPS-1))
		i++;
	unsigned long arm_pc = (R15 & ~PAGING::ADDRESS_MASK) + (i << U::INSTRUCTION_SIZE_LG2);

	// discover the JIT subinstruction using distorm through the breakpoint interface
	breakpoint_defs::break_data bd;
	bd.addr = arm_pc;
	breakpoints<T, U>::disassemble_breakdata(&bd);
	
	unsigned long arm_jit = 0;
	while ((eip >= bd.jit_line[arm_jit+1].pos) && (arm_jit < bd.jit_instructions-1))
		arm_jit++;

	exception_context<T>::context.addr_resolved = true;
	exception_context<T>::context.addr = arm_pc;
	exception_context<T>::context.subaddr = arm_jit;
	return true;
}


template <typename T>
bool resolve_armpc_from_eip(void *eip)
{
	unsigned long R15 = processor<T>::context.regs[15];
	if (R15 & 1)
		return resolve_armpc_from_eip2<T, IS_THUMB>(R15, eip);
	else
		return resolve_armpc_from_eip2<T, IS_ARM>(R15, eip);
}

template <typename T>
char* resolve_eip()
{
#pragma warning(push)
#pragma warning(disable: 4311)
#pragma warning(disable: 4312)

	void *p = (void*)CONTEXT_EBP(exception_context<T>::context.ctx.uc_mcontext);
	void **lastp = 0;
	exception_context<T>::context.addr_resolved = false;

	// if ebp still is the context base we are either in emu JIT
	// or in a HLE that hasnt a stackframe (handle that case somehow)
	if (p == &processor<T>::context)
	{
		char *eip = (char*)CONTEXT_EIP(exception_context<T>::context.ctx.uc_mcontext);
		resolve_armpc_from_eip<T>(eip);
		return eip;
	}
	logging<T>::logf("A HLE or OS call crashed emulation");

	// scan till return is found
	while (p != &processor<T>::context)
	{
		lastp = (void**)p;
		if ( !check_mem_access( lastp, 2*sizeof(void*)) )
			return (char*)CONTEXT_EIP(exception_context<T>::context.ctx.uc_mcontext); // ebp has been screwed up
		p = *lastp;
	}
	void *eip = lastp[1];
	CONTEXT_EIP(exception_context<T>::context.ctx.uc_mcontext) = (unsigned long)eip;

	// finally try to retrieve the ARM address of the crash assuming it happened within
	// the block R15 points to
	resolve_armpc_from_eip<T>(eip);
	return (char*)eip;
#pragma warning(pop)
}

template <typename T> struct runner
{
	enum { STACK_SIZE = 4096*64 }; // 64K reserved for JIT + HLE funcs
	static Fiber *fiber;
	static Fiber::fiber_cb *cb;
	static breakpoint_defs::break_info *repatch;
	static const source_set* skipsrc;
	static breakpoint_defs::stepmode skipmode;
	typedef void (*jit_function)();



	template <typename U>
	static jit_function get_entry(unsigned long addr)
	{
		processor<T>::context.regs[15] = addr & (~PAGING::ADDRESS_MASK);
		memory_block *b = memory_map<T>::addr2page( addr );
		if (b->flags & (memory_block::PAGE_INVALID | memory_block::PAGE_EXECPROT))
			return 0;

		if (!b->get_jit<T, U>())
			b->recompile<T, U>();
		compiled_block<U> *code = b->get_jit<T, U>();

		unsigned long subaddr = addr & PAGING::ADDRESS_MASK;
		char *start = code->remap[subaddr >> U::INSTRUCTION_SIZE_LG2];
		//FlushInstructionCache( GetCurrentProcess(), start, code->code_size - (start - code->code) );
		cacheflush( start, (int)(code->code_size - (start - code->code)), ICACHE );
		return (jit_function)start;
	}

	static bool jit_continue()
	{
		// TODO: ARM memory might have been invalidated meantime
		// check if break was on a ARM line and recompile if needed
		// this has quite some problems though

		// if there still is a breakpoint at the current location
		// unpatch it single step in host using TF
		// and then repatch the BP finally continue execution
		// the repatch is done in the fiber_cb
		breakpoint_defs::break_info *bi =
			breakpoints_base<T>::resolve( (char*)UlongToPtr(CONTEXT_EIP(fiber->context.uc_mcontext)) );
		if (bi && bi->patched)
		{
			repatch = bi;
			*bi->pos = bi->original_byte;
			CONTEXT_EFLAGS(fiber->context.uc_mcontext) |= (1 << 8); // set trap flag
		}

		fiber->do_continue();
		return true;
	}

	static void jit_rebranch(unsigned long addr)
	{
		// resolve new entrypoint
		jit_function jit_code;
		if (addr & 1)
			jit_code = get_entry<IS_THUMB>(addr);
		else
			jit_code = get_entry<IS_ARM>(addr);
		if (!jit_code)
			return;

		// set EIP and R15 according to addr
		processor<T>::context.regs[15] = addr;
		CONTEXT_EIP(fiber->context.uc_mcontext) = PtrToUlong(jit_code);
		CONTEXT_EBP(fiber->context.uc_mcontext) = PtrToUlong(&processor<T>::context);
	}

	static void internal_cb(Fiber *f)
	{
		static bool initialized = false;
		if (repatch)
		{
			// single step caused this
			*repatch->pos = DEBUG_BREAK;
			repatch = 0;
			f->do_continue();
			return;
		}

		exception_context<T>::context.ctx = f->context;
		if (initialized)
			breakpoints_base<T>::trigger( resolve_eip<T>() );
		else initialized = true;
		if (skipsrc)
			if (skipcb())
				return;
		cb(f);
	}

	static void jit_init(Fiber::fiber_cb c)
	{
		cb = c;
		fiber = Fiber::create( STACK_SIZE, internal_cb);
	}

	static bool skipcb()
	{
		// check if current trigger was in the source set
		// if so continue stepping
		breakpoint_defs::break_info *bi = breakpoints_base<T>::get_last_error();
		if (bi)
		{
			unsigned long addr = bi->block->addr;
			// if the address is within the source set continue stepping
			for (source_set::info_set::const_iterator it =
				skipsrc->set.begin(); it != skipsrc->set.end(); ++it)
			{
				source_info *si = *it;
				if ((addr >= si->lopc) && (addr < si->hipc)) // inside block?
				{
					// continue
					if (processor<T>::context.regs[15] & 1)
						breakpoints<T, IS_THUMB>::step(skipmode);
					else breakpoints<T, IS_ARM>::step(skipmode);
					jit_continue();
					return true;
				}
			}
		}
		return false;
	}

	static void skipline(const source_set *line, breakpoint_defs::stepmode mode)
	{
		skipsrc = line;
		skipmode = mode;
		skipcb();
	}

};

template <typename T> Fiber* runner<T>::fiber;
template <typename T> Fiber::fiber_cb* runner<T>::cb;
template <typename T> breakpoint_defs::break_info* runner<T>::repatch = 0;
template <typename T> const source_set* runner<T>::skipsrc = 0;
template <typename T> breakpoint_defs::stepmode runner<T>::skipmode;

#endif
