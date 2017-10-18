#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target/target.h"
#include "target/register.h"
#include "riscv.h"
#include "program.h"
#include "helper/log.h"

#include "asm.h"
#include "encoding.h"

riscv_addr_t riscv_program_gal(struct riscv_program *p, riscv_addr_t addr);
int riscv_program_lah(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr);
int riscv_program_lal(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr);

/* Program interface. */
int riscv_program_init(struct riscv_program *p, struct target *target)
{
	memset(p, 0, sizeof(*p));
	p->target = target;
	p->instruction_count = 0;
	p->writes_memory = 0;
	p->target_xlen = riscv_xlen(target);
	for (size_t i = 0; i < RISCV_REGISTER_COUNT; ++i) {
		p->writes_xreg[i] = 0;
		p->in_use[i] = 0;
	}

	for(size_t i = 0; i < RISCV_MAX_DEBUG_BUFFER_SIZE; ++i)
		p->debug_buffer[i] = -1;

	return ERROR_OK;
}

int riscv_program_write(struct riscv_program *program)
{
	for (unsigned i = 0; i < program->instruction_count; ++i) {
		LOG_DEBUG("%p: debug_buffer[%02x] = DASM(0x%08x)", program, i, program->debug_buffer[i]);
		if (riscv_write_debug_buffer(program->target, i,
					program->debug_buffer[i]) != ERROR_OK)
			return ERROR_FAIL;
	}
	return ERROR_OK;
}

/** Add ebreak and execute the program. */
int riscv_program_exec(struct riscv_program *p, struct target *t)
{
	keep_alive();

	riscv_reg_t saved_registers[GDB_REGNO_XPR31 + 1];
	for (size_t i = GDB_REGNO_XPR0 + 1; i <= GDB_REGNO_XPR31; ++i) {
		if (p->writes_xreg[i]) {
			LOG_DEBUG("Saving register %d as used by program", (int)i);
			saved_registers[i] = riscv_get_register(t, i);
		}
	}

	if (p->writes_memory && (riscv_program_fence(p) != ERROR_OK)) {
		LOG_ERROR("Unable to write fence");
		for(size_t i = 0; i < riscv_debug_buffer_size(p->target); ++i)
			LOG_ERROR("ram[%02x]: DASM(0x%08lx) [0x%08lx]", (int)i, (long)p->debug_buffer[i], (long)p->debug_buffer[i]);
		abort();
		return ERROR_FAIL;
	}

	if (riscv_program_ebreak(p) != ERROR_OK) {
		LOG_ERROR("Unable to write ebreak");
		for(size_t i = 0; i < riscv_debug_buffer_size(p->target); ++i)
			LOG_ERROR("ram[%02x]: DASM(0x%08lx) [0x%08lx]", (int)i, (long)p->debug_buffer[i], (long)p->debug_buffer[i]);
		abort();
		return ERROR_FAIL;
	}

	if (riscv_program_write(p) != ERROR_OK)
		return ERROR_FAIL;

	if (riscv_execute_debug_buffer(t) != ERROR_OK) {
		LOG_ERROR("Unable to execute program %p", p);
		return ERROR_FAIL;
	}

	for (size_t i = 0; i < riscv_debug_buffer_size(p->target); ++i)
		if (i >= riscv_debug_buffer_size(p->target))
			p->debug_buffer[i] = riscv_read_debug_buffer(t, i);

	for (size_t i = GDB_REGNO_XPR0; i <= GDB_REGNO_XPR31; ++i)
		if (p->writes_xreg[i])
			riscv_set_register(t, i, saved_registers[i]);

	return ERROR_OK;
}

int riscv_program_swr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int offset)
{
	p->writes_memory = 1;
	return riscv_program_insert(p, sw(d, b, offset));
}

int riscv_program_shr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int offset)
{
	p->writes_memory = 1;
	return riscv_program_insert(p, sh(d, b, offset));
}

int riscv_program_sbr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int offset)
{
	p->writes_memory = 1;
	return riscv_program_insert(p, sb(d, b, offset));
}

int riscv_program_lwr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int offset)
{
	p->writes_memory = 1;
	return riscv_program_insert(p, lw(d, b, offset));
}

int riscv_program_lhr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int offset)
{
	p->writes_memory = 1;
	return riscv_program_insert(p, lh(d, b, offset));
}

int riscv_program_lbr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int offset)
{
	p->writes_memory = 1;
	return riscv_program_insert(p, lb(d, b, offset));
}

int riscv_program_lx(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	switch (p->target_xlen) {
	case 64:  return riscv_program_ld(p, d, addr);
	case 32:  return riscv_program_lw(p, d, addr);
	}

	LOG_ERROR("unknown xlen %d", p->target_xlen);
	abort();
	return -1;
}

int riscv_program_ld(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0 ? GDB_REGNO_X0 : d;
	if (riscv_program_lah(p, d, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, ld(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	return ERROR_OK;
}

int riscv_program_lw(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0 ? GDB_REGNO_X0 : d;
	if (riscv_program_lah(p, d, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, lw(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	return ERROR_OK;
}

int riscv_program_lh(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0 ? GDB_REGNO_X0 : d;
	if (riscv_program_lah(p, d, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, lh(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	return ERROR_OK;
}

int riscv_program_lb(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0 ? GDB_REGNO_X0 : d;
	if (riscv_program_lah(p, t, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, lb(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	return ERROR_OK;
}

int riscv_program_sx(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	switch (p->target_xlen) {
	case 64:  return riscv_program_sd(p, d, addr);
	case 32:  return riscv_program_sw(p, d, addr);
	}

	LOG_ERROR("unknown xlen %d", p->target_xlen);
	abort();
	return -1;
}

int riscv_program_sd(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0
		? GDB_REGNO_X0
		: riscv_program_gettemp(p);
	if (riscv_program_lah(p, t, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, sd(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	riscv_program_puttemp(p, t);
	p->writes_memory = true;
	return ERROR_OK;
}

int riscv_program_sw(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0
		? GDB_REGNO_X0
		: riscv_program_gettemp(p);
	if (riscv_program_lah(p, t, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, sw(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	riscv_program_puttemp(p, t);
	p->writes_memory = true;
	return ERROR_OK;
}

int riscv_program_sh(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0
		? GDB_REGNO_X0
		: riscv_program_gettemp(p);
	if (riscv_program_lah(p, t, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, sh(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	riscv_program_puttemp(p, t);
	p->writes_memory = true;
	return ERROR_OK;
}

int riscv_program_sb(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	enum gdb_regno t = riscv_program_gah(p, addr) == 0
		? GDB_REGNO_X0
		: riscv_program_gettemp(p);
	if (riscv_program_lah(p, t, addr) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_insert(p, sb(d, t, riscv_program_gal(p, addr))) != ERROR_OK)
		return ERROR_FAIL;
	riscv_program_puttemp(p, t);
	p->writes_memory = true;
	return ERROR_OK;
}

int riscv_program_csrr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno csr)
{
	assert(csr >= GDB_REGNO_CSR0 && csr <= GDB_REGNO_CSR4095);
	return riscv_program_insert(p, csrrs(d, GDB_REGNO_X0, csr - GDB_REGNO_CSR0));
}

int riscv_program_csrw(struct riscv_program *p, enum gdb_regno s, enum gdb_regno csr)
{
	assert(csr >= GDB_REGNO_CSR0);
	return riscv_program_insert(p, csrrw(GDB_REGNO_X0, s, csr - GDB_REGNO_CSR0));
}

int riscv_program_csrrw(struct riscv_program *p, enum gdb_regno d, enum gdb_regno s, enum gdb_regno csr)
{
	assert(csr >= GDB_REGNO_CSR0);
	return riscv_program_insert(p, csrrw(d, s, csr - GDB_REGNO_CSR0));
}

int riscv_program_fence_i(struct riscv_program *p)
{
	return riscv_program_insert(p, fence_i());
}

int riscv_program_fence(struct riscv_program *p)
{
	return riscv_program_insert(p, fence());
}

int riscv_program_ebreak(struct riscv_program *p)
{
	if (p->instruction_count == riscv_debug_buffer_size(p->target)) {
		// TODO: Check for impebreak bit.
		// There's an implicit ebreak here, so no need for us to add one.
		return ERROR_OK;
	}
	return riscv_program_insert(p, ebreak());
}

int riscv_program_lui(struct riscv_program *p, enum gdb_regno d, int32_t u)
{
	return riscv_program_insert(p, lui(d, u));
}

int riscv_program_addi(struct riscv_program *p, enum gdb_regno d, enum gdb_regno s, int16_t u)
{
	return riscv_program_insert(p, addi(d, s, u));
}

int riscv_program_li(struct riscv_program *p, enum gdb_regno d, riscv_reg_t c)
{
	if (riscv_program_lui(p, d, c >> 12) != ERROR_OK)
		return ERROR_FAIL;
	if (riscv_program_addi(p, d, d, c & 0xFFF) != ERROR_OK)
		return ERROR_FAIL;
	return ERROR_OK;
}

int riscv_program_dont_restore_register(struct riscv_program *p, enum gdb_regno r)
{
	assert(r < RISCV_REGISTER_COUNT);
	p->writes_xreg[r] = 0;
	return ERROR_OK;
}

int riscv_program_do_restore_register(struct riscv_program *p, enum gdb_regno r)
{
	assert(r < RISCV_REGISTER_COUNT);
	p->writes_xreg[r] = 1;
	return ERROR_OK;
}

enum gdb_regno riscv_program_gettemp(struct riscv_program *p)
{
	for (size_t i = GDB_REGNO_S0; i <= GDB_REGNO_XPR31; ++i) {
		if (p->in_use[i]) continue;

		riscv_program_do_restore_register(p, i);
		p->in_use[i] = 1;
		return i;
	}

	LOG_ERROR("You've run out of temporary registers.  This is impossible.");
	abort();
}

void riscv_program_puttemp(struct riscv_program *p, enum gdb_regno r)
{
	assert(r < RISCV_REGISTER_COUNT);
	p->in_use[r] = 0;
}

/* Helper functions. */
riscv_addr_t riscv_program_gah(struct riscv_program *p, riscv_addr_t addr)
{
	return addr >> 12;
}

riscv_addr_t riscv_program_gal(struct riscv_program *p, riscv_addr_t addr)
{
	if (addr > 0) {
	    return (addr & 0x7FF);
	} else {
	    return 0;
	}
}

int riscv_program_lah(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	riscv_addr_t ah = riscv_program_gah(p, addr);
	if (ah == 0)
		return ERROR_OK;
	return riscv_program_lui(p, d, ah);
}

int riscv_program_lal(struct riscv_program *p, enum gdb_regno d, riscv_addr_t addr)
{
	riscv_addr_t al = riscv_program_gal(p, addr);
	if (al == 0)
		return ERROR_OK;
	return riscv_program_addi(p, d, d, al);
}

int riscv_program_insert(struct riscv_program *p, riscv_insn_t i)
{
	if (p->instruction_count >= riscv_debug_buffer_size(p->target)) {
		LOG_ERROR("Unable to insert instruction:");
		LOG_ERROR("  instruction_count=%d", (int)p->instruction_count);
		LOG_ERROR("  buffer size      =%d", (int)riscv_debug_buffer_size(p->target));
		abort();
	}

	p->debug_buffer[p->instruction_count] = i;
	p->instruction_count++;
	return ERROR_OK;
}
