/* ============================================================================
 * dpu_interp.cpp  --  an interpreter for generated DPU programs
 *
 * Purpose: translation validation.  The simulator can execute an offloaded nest
 * either from the IR (the compiler's intermediate view) or by RUNNING THE
 * GENERATED DPU PROGRAM -- after register allocation, with only K physical
 * registers and real spill slots.  If the two agree bit-for-bit with the host,
 * the instruction selection, the scratchpad tiling and the Chaitin-Briggs
 * allocator all preserved the program's meaning.
 *
 * Machine model of one DPU: a register file of typed values, a spill area, and a
 * scratchpad holding at most the elements brought in by the latest DMA_IN of
 * each array (a DMA_IN replaces that array's window).  Loading an element that
 * is not resident, or running past an array, is an error.  DMA_OUT writes back
 * only the elements the program stored to.
 * ==========================================================================*/
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"
#include "dpu.h"

struct Win {
	std::vector<Value> data;
	std::vector<char>  valid;
	std::vector<char>  dirty;
	std::vector<int>   dims;
	TypeTag            type;
	long               rowelems;
	bool               is_private;
};

static bool truthy_v(Value v)
{
	return v.type == TY_INT ? v.i != 0 : v.f != 0.0f;
}

bool dpu_interpret(IRProgram *p, DProgram *prog, DpuLaunch *launch, DpuRun *run)
{
	size_t n = prog->code.size();
	std::vector<int> match(n, -1), else_of(n, -1);
	std::vector<int> st;
	size_t i;
	run->instructions = 0;
	run->err.clear();

	for (i = 0; i < n; i++) {
		int op = prog->code[i].op;
		if (op == D_LOOP || op == D_IFNZ)
			st.push_back((int)i);
		else if (op == D_ELSE)
			else_of[st.back()] = (int)i;
		else if (op == D_ENDLOOP || op == D_ENDIF) {
			match[st.back()] = (int)i;
			match[i] = st.back();
			st.pop_back();
		}
	}

	int nreg = std::max(prog->nvregs, prog->nregs) + 1;
	std::vector<Value> regs((size_t)nreg, mk_val_int(0));
	std::vector<Value> spill((size_t)prog->nspill + 1, mk_val_int(0));
	std::map<std::string, Win> win;

	size_t pc = 0;
	while (pc < n) {
		DInstr &I = prog->code[pc];
		if (++run->instructions > 400000000L) {
			run->err = "instruction limit exceeded (runaway program)";
			return false;
		}
		switch (I.op) {
		case D_MOVI:
			regs[I.d] = I.fp ? mk_val_float((float)I.fimm) : mk_val_int(I.imm);
			break;
		case D_MOV:
			regs[I.d] = value_convert(regs[I.a], I.fp ? TY_FLOAT : TY_INT);
			break;
		case D_MIN:
			regs[I.d] = (regs[I.a].i < regs[I.b].i) ? regs[I.a] : regs[I.b];
			break;
		case D_ADD: case D_SUB: case D_MUL: case D_DIV: case D_MOD: {
			static const int map_op[] = {OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD};
			Value r = eval_binop(map_op[I.op - D_ADD], regs[I.a], regs[I.b]);
			regs[I.d] = I.fp ? value_convert(r, TY_FLOAT) : r;
			break;
		}
		case D_SLT: case D_SLE: case D_SGT: case D_SGE: case D_SEQ: case D_SNE: {
			static const int map_op[] = {OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE};
			regs[I.d] = eval_binop(map_op[I.op - D_SLT], regs[I.a], regs[I.b]);
			break;
		}
		case D_LD: {
			std::map<std::string, Win>::iterator wi = win.find(I.sym);
			long idx = regs[I.a].i;
			if (wi == win.end() || idx < 0 || idx >= (long)wi->second.data.size()) {
				run->err = "LD " + I.sym + ": index out of range or array not in the scratchpad";
				return false;
			}
			if (!wi->second.valid[(size_t)idx]) {
				char b[96];
				snprintf(b, sizeof b, "LD %s[%ld]: element not resident in the scratchpad", I.sym.c_str(), idx);
				run->err = b;
				return false;
			}
			regs[I.d] = wi->second.data[(size_t)idx];
			break;
		}
		case D_ST: {
			std::map<std::string, Win>::iterator wi = win.find(I.sym);
			long idx = regs[I.a].i;
			if (wi == win.end()) {
				/* write-only arrays have no DMA_IN: create their window lazily */
				ArrayInfo &ai = p->arrays[I.sym];
				Win w;
				long total = 1;
				for (size_t d = 0; d < ai.dims.size(); d++)
					total *= ai.dims[d];
				w.dims = ai.dims;
				w.type = ai.elem_type;
				w.rowelems = total / ai.dims[0];
				w.data.assign((size_t)total, w.type == TY_FLOAT ? mk_val_float(0) : mk_val_int(0));
				w.valid.assign((size_t)total, 0);
				w.dirty.assign((size_t)total, 0);
				w.is_private = false;
				win[I.sym] = w;
				wi = win.find(I.sym);
			}
			if (idx < 0 || idx >= (long)wi->second.data.size()) {
				run->err = "ST " + I.sym + ": index out of range";
				return false;
			}
			wi->second.data[(size_t)idx] = value_convert(regs[I.b], wi->second.type);
			wi->second.valid[(size_t)idx] = 1;
			wi->second.dirty[(size_t)idx] = 1;
			break;
		}
		case D_DMA_IN: {
			ArrayInfo &ai = p->arrays[I.sym];
			std::vector<Value> *g = run->mram[I.sym];
			long total = (long)g->size();
			long rowelems = total / ai.dims[0];
			Win &w = win[I.sym];
			if (w.data.empty()) {
				w.dims = ai.dims;
				w.type = ai.elem_type;
				w.rowelems = rowelems;
				w.is_private = false;
				w.data.assign((size_t)total, w.type == TY_FLOAT ? mk_val_float(0) : mk_val_int(0));
				w.dirty.assign((size_t)total, 0);
			}
			w.valid.assign((size_t)total, 0);               /* a DMA_IN replaces the window */
			long r0 = regs[I.a].i, nr = regs[I.b].i, rr;
			for (rr = std::max(0L, r0); rr < std::min((long)ai.dims[0], r0 + nr); rr++)
				for (long e = 0; e < rowelems; e++) {
					size_t k = (size_t)(rr * rowelems + e);
					if (!w.dirty[k])
						w.data[k] = (*g)[k];
					w.valid[k] = 1;
				}
			break;
		}
		case D_DMA_OUT: {
			std::map<std::string, Win>::iterator wi = win.find(I.sym);
			if (wi == win.end())
				break;
			ArrayInfo &ai = p->arrays[I.sym];
			Win &w = wi->second;
			long r0 = regs[I.a].i, nr = regs[I.b].i, rr;
			std::vector<Value> *g = run->mram[I.sym];
			for (rr = std::max(0L, r0); rr < std::min((long)ai.dims[0], r0 + nr); rr++)
				for (long e = 0; e < w.rowelems; e++) {
					size_t k = (size_t)(rr * w.rowelems + e);
					if (!w.dirty[k] && !w.is_private)
						continue;
					if (w.is_private) {
						std::vector<Value> &po = run->priv_out[I.sym];
						if (po.size() != w.data.size())
							po.assign(w.data.size(), mk_val_int(0));
						po[k] = w.data[k];
					} else {
						(*g)[k] = w.data[k];
						w.dirty[k] = 0;
					}
				}
			break;
		}
		case D_HALO:
			break;
		case D_FILL: {
			ArrayInfo &ai = p->arrays[I.sym];
			long total = 1;
			for (size_t d = 0; d < ai.dims.size(); d++)
				total *= ai.dims[d];
			Win &w = win[I.sym];
			w.dims = ai.dims;
			w.type = ai.elem_type;
			w.rowelems = total / ai.dims[0];
			w.is_private = true;
			Value id = I.fp ? mk_val_float((float)I.fimm) : mk_val_int(I.imm);
			w.data.assign((size_t)total, value_convert(id, w.type));
			w.valid.assign((size_t)total, 1);
			w.dirty.assign((size_t)total, 0);
			break;
		}
		case D_ARG: {
			if (I.sym == "first")       regs[I.d] = mk_val_int(launch->first);
			else if (I.sym == "count")  regs[I.d] = mk_val_int(launch->count);
			else if (I.sym == "stride") regs[I.d] = mk_val_int(launch->stride);
			else {
				std::map<std::string, Value>::iterator it = launch->scalars.find(I.sym);
				if (it == launch->scalars.end()) {
					run->err = "missing launch argument " + I.sym;
					return false;
				}
				regs[I.d] = it->second;
			}
			break;
		}
		case D_RET:
			run->results[I.sym] = regs[I.a];
			break;
		case D_SPILL_LD:
			regs[I.d] = spill[(size_t)I.imm];
			break;
		case D_SPILL_ST:
			spill[(size_t)I.imm] = regs[I.a];
			break;
		case D_LOOP:
			if (!(regs[I.a].i < regs[I.b].i)) {
				pc = (size_t)match[pc] + 1;
				continue;
			}
			break;
		case D_ENDLOOP:
			pc = (size_t)match[pc];
			continue;
		case D_IFNZ:
			if (!truthy_v(regs[I.a])) {
				pc = else_of[pc] >= 0 ? (size_t)else_of[pc] + 1 : (size_t)match[pc];
				continue;
			}
			break;
		case D_ELSE: {
			/* end of the then-branch: jump to the matching ENDIF */
			size_t j = pc + 1;
			int nest = 0;
			for (; j < n; j++) {
				if (prog->code[j].op == D_IFNZ) nest++;
				if (prog->code[j].op == D_ENDIF) {
					if (nest == 0) break;
					nest--;
				}
			}
			pc = j;
			continue;
		}
		case D_ENDIF:
			break;
		}
		pc++;
	}
	return true;
}
