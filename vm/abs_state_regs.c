/*
 * Copyright 2018 VMware, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <assert.h>

#include "ubpf_int.h"
#include "abs_dom.h"
#include "abs_interp.h"

void 
abs_initialize_entry(struct abs_state *state)
{
    for (int i = 0; i < 11; i++) {
        state->reg[i] = abs_top;
    }
}

void 
abs_initialize_unreached(struct abs_state *state)
{
    state->bot = true;
}

void
abs_join(struct abs_state *state, struct abs_state other)
{
    if (state->bot) {
        *state = other;
    } else {
        for (int r = 1; r < 11; r++) {
            state->reg[r] = abs_const_join(state->reg[r], other.reg[r]);
        }
    }
}

static uint32_t
u32(uint64_t x)
{
    return x;
}

static int
access_width(uint8_t opcode) {
    switch (opcode) {
    case EBPF_OP_LDXB:
    case EBPF_OP_STB:
    case EBPF_OP_STXB: return 1;
    case EBPF_OP_LDXH:
    case EBPF_OP_STH:
    case EBPF_OP_STXH: return 2;
    case EBPF_OP_LDXW:
    case EBPF_OP_STW:
    case EBPF_OP_STXW: return 4;
    case EBPF_OP_LDXDW:
    case EBPF_OP_STDW:
    case EBPF_OP_STXDW: return 8;
    default: return -1;
    }
}

bool
abs_bounds_fail(struct abs_state *state, struct ebpf_inst inst, uint16_t pc, char** errmsg) {
    int width = access_width(inst.opcode);
    if (width <= 0) {
        return false;
    }

    bool is_load = ((inst.opcode & EBPF_CLS_MASK) == EBPF_CLS_LD)
                || ((inst.opcode & EBPF_CLS_MASK) == EBPF_CLS_LDX);
    uint8_t r = is_load ? inst.src : inst.dst;
    bool fail;
    if (r == 10) {
        fail = inst.offset + width > 0 || inst.offset < -STACK_SIZE;
    } else if (r == 1) {
        // unsafely assume this is context pointer
        fail = inst.offset < 0 || inst.offset + width > 4096;
    } else {
        fail = true;
    }
    if (fail) {
        *errmsg = ubpf_error("out of bounds memory %s at PC %d [r%d%+d]",
                            is_load ? "load" : "store", pc, is_load ? inst.src : inst.dst, inst.offset);
    }
    return fail;
}

bool
abs_divzero_fail(struct abs_state *state, struct ebpf_inst inst, uint16_t pc, char** errmsg) {
    bool div = (inst.opcode & EBPF_ALU_OP_MASK) == (EBPF_OP_DIV_REG & EBPF_ALU_OP_MASK);
    bool mod = (inst.opcode & EBPF_ALU_OP_MASK) == (EBPF_OP_MOD_REG & EBPF_ALU_OP_MASK);
    if (div || mod) {
        bool is64 = (inst.opcode & EBPF_CLS_MASK) == EBPF_CLS_ALU64;
        if (!state->reg[inst.src].known
        || ( is64 &&     state->reg[inst.src].value  == 0)
        || (!is64 && u32(state->reg[inst.src].value) == 0)) {
            *errmsg = ubpf_error("division by zero at PC %d", pc);
            return true;
        }
    }
    return false;
}

static bool
is_mov(uint8_t opcode)
{
    return opcode == EBPF_OP_MOV64_IMM
        || opcode == EBPF_OP_MOV64_REG
        || opcode == EBPF_OP_MOV_IMM
        || opcode == EBPF_OP_MOV_REG;
}

static bool
is_alu(uint8_t opcode)
{
    return (opcode & EBPF_CLS_MASK) == EBPF_CLS_ALU
        || (opcode & EBPF_CLS_MASK) == EBPF_CLS_ALU64;
}

void
abs_execute(struct abs_state *to, struct abs_state *from, struct ebpf_inst inst, int32_t imm)
{
    // (it can be an optimization for a specific domain, with default implementation as execute then join)
    struct abs_state state = *from;

    if (inst.opcode == EBPF_OP_LDDW) {
        state.reg[inst.dst].value = (uint32_t)inst.imm | ((uint64_t)imm << 32);
    } else if (inst.opcode == EBPF_OP_CALL) {
        for (int r=1; r <= 5; r++) {
            state.reg[r].known = false;
        }
        // r0 depends on the particular function
        state.reg[0].known = false;
    } else if (!is_alu(inst.opcode)) {
        
    } else if (inst.opcode & EBPF_SRC_REG && !state.reg[inst.src].known) {
        state.reg[inst.dst].known = false;
    } else if (!state.reg[inst.dst].known && !is_mov(inst.opcode)) {
        // if it's not mov, the dst register is also important for definedness
        state.reg[inst.dst].known = false;
    } else {
        state.reg[inst.dst].known = true;
        state.reg[inst.dst].value = do_const_alu(inst.opcode, inst.imm, state.reg[inst.dst].value, state.reg[inst.src].value);
    }

    abs_join(to, state);
}

void
abs_execute_assume(struct abs_state *to, struct abs_state *from, struct ebpf_inst inst, bool taken)
{
    struct abs_state state = *from;

    // TODO: check feasibility; this might cause problems with pending.
    if ((taken && inst.opcode == EBPF_OP_JEQ_IMM)
    || (!taken && inst.opcode == EBPF_OP_JNE_IMM)) {
        state.reg[inst.dst].known = true;
        state.reg[inst.dst].value = inst.imm;
    }
    if ((taken && inst.opcode == EBPF_OP_JEQ_REG)
    || (!taken && inst.opcode == EBPF_OP_JNE_REG)) {
        state.reg[inst.dst].known = true;
        state.reg[inst.dst].value = state.reg[inst.src].value;
        // we don't track correlation
    }
    
    abs_join(to, state);
}
