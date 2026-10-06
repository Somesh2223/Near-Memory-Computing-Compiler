/* ============================================================================
 * dpu.h  --  the PIM instruction set, DPU programs, register allocation,
 *            code generation and an interpreter for generated programs
 *
 * The DPU ISA (every DPU runs the same program on its own slice of the data):
 *
 *   arithmetic   ADD SUB MUL DIV MOD  rd, ra, rb      (".F" = float operation)
 *   compare      SLT SLE SGT SGE SEQ SNE rd, ra, rb   (rd = 0 or 1)
 *   moves        MOVI rd, imm      MOV rd, ra      MIN rd, ra, rb
 *   scratchpad   LD rd, X[ri]      ST X[ri], rb     (ri = global element index of
 *                the array X; the runtime maps it through the window that the
 *                latest DMA_IN of X placed in the scratchpad)
 *   transfers    DMA_IN X, row0, nrows     bank -> scratchpad, rows outside the
 *                                          array are ignored
 *                DMA_OUT X, row0, nrows    scratchpad -> bank, only elements the
 *                                          DPU actually stored to
 *                HALO_EXCHANGE X, rows     fetch `rows` boundary rows owned by the
 *                                          neighbouring DPUs
 *   control      LOOP ri, rbound ... ENDLOOP   (while ri < rbound; the body adds
 *                                          the step to ri explicitly, ENDLOOP is a
 *                                          pure branch back to LOOP)
 *                IFNZ rc ... ELSE ... ENDIF
 *   interface    ARG rd, name     RET name, ra      (launch arguments / results)
 *
 * MIN, the compares, MOV, ARG, RET, FILL and the ".F" variants are additions to
 * the minimal list in the course brief; they are needed to express conditionals,
 * min-reductions and typed arithmetic.
 * ==========================================================================*/
#ifndef NEMOC_DPU_H
#define NEMOC_DPU_H

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ir.h"
#include "plan.h"

enum DOp {
	D_MOVI, D_MOV, D_MIN,
	D_ADD, D_SUB, D_MUL, D_DIV, D_MOD,
	D_SLT, D_SLE, D_SGT, D_SGE, D_SEQ, D_SNE,
	D_LD, D_ST,
	D_DMA_IN, D_DMA_OUT, D_HALO, D_FILL,
	D_LOOP, D_ENDLOOP, D_IFNZ, D_ELSE, D_ENDIF,
	D_ARG, D_RET,
	D_SPILL_LD, D_SPILL_ST
};

struct DInstr {
	int         op;
	bool        fp;          /* float variant of ADD/SUB/MUL/DIV/compare/MOVI   */
	int         d;           /* destination register  (-1 none)                 */
	int         a;           /* first source register (-1 none)                 */
	int         b;           /* second source register (-1 none)                */
	long        imm;         /* integer immediate / loop step / spill slot / row bytes */
	double      fimm;        /* float immediate for MOVI.F                      */
	std::string sym;         /* array / argument / result name                  */
	std::string note;        /* trailing comment in the listing                 */
};

struct DProgram {
	std::string              name;
	std::vector<DInstr>      code;
	std::vector<std::string> vreg_name;   /* debugging name of each virtual register */
	std::vector<bool>        vreg_float;
	int                      nvregs;
	bool                     allocated;   /* operands are physical registers now     */
	int                      nregs;       /* physical registers available (K)        */
	int                      nspill;      /* spill slots used                        */
	std::vector<std::string> header;      /* comment lines at the top of the listing */
	/* launch-interface facts the host and the interpreter need */
	long                     tile_rows;
	bool                     strided;     /* cyclic distribution: iterations first+k*stride */
};

/* ---- register allocation (regalloc.cpp): Chaitin-Briggs graph colouring ------- */
struct LiveRange {
	int vreg;
	int first, last;       /* first and last instruction where it is live */
	int color;             /* -1 = spilled                                  */
};

struct RAResult {
	int                       K;
	std::vector<int>          color;       /* per virtual register                   */
	std::vector<int>          order;       /* the order registers were coloured in   */
	std::vector<std::pair<int,int> > coalesced;
	std::vector<LiveRange>    ranges;
	int                       rounds;      /* build/simplify/select rounds            */
	int                       spilled;     /* registers spilled to the scratchpad     */
	int                       registers_used;
	bool                      verified;    /* no two interfering registers share a colour */
	/* the interference graph of the FINAL round, for --emit-dot interference */
	std::vector<std::vector<int> > graph;
	std::vector<int>          graph_color;
};

void regalloc_run(DProgram *prog, int K, RAResult *out, int verbose);
void regalloc_print(DProgram *prog, RAResult *r);
void regalloc_emit_dot(DProgram *prog, RAResult *r, FILE *out);

/* ---- code generation ------------------------------------------------------------ */
/* build the DPU program of one offloaded nest (virtual registers, not yet allocated) */
bool dpu_codegen(IRProgram *p, NestPlan *np, DProgram *out, std::string *err);
void dpu_print(DProgram *prog, FILE *out);
long dpu_tile_rows(IRProgram *p, NestPlan *np, std::string *arithmetic);

/* codegen_host.cpp: host orchestration C for one kernel; returns the file text */
std::string host_codegen(ProgramPlan *pp, KernelPlan *kp, std::vector<DProgram*> &dprogs);

/* ---- interpreter for generated programs (dpu_interp.cpp) ------------------------- */
struct DpuLaunch {
	long                         first, count, stride;
	std::map<std::string, Value> scalars;      /* launch arguments                       */
};

struct DpuRun {
	std::map<std::string, std::vector<Value>*>  mram;      /* the arrays in the bank (global)  */
	std::map<std::string, std::vector<Value> >  priv_out;  /* private arrays returned          */
	std::map<std::string, Value>                results;   /* RET values                       */
	std::string                                 err;
	long                                        instructions;
};

bool dpu_interpret(IRProgram *p, DProgram *prog, DpuLaunch *launch, DpuRun *run);

/* all DPU programs of one plan, generated and register-allocated; used by the
 * driver and the tests.  Returns the number of programs generated. */
int dpu_generate_all(ProgramPlan *pp, int K, std::vector<DProgram*> *out, int verbose);

#endif /* NEMOC_DPU_H */
