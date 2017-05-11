/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
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
 *
 */

/** \brief Fortran transformation module */

#include "gbldefs.h"
#include "global.h"
#include "error.h"
#include "comm.h"
#include "symtab.h"
#include "symutl.h"
#include "dtypeutl.h"
#include "soc.h"
#include "semant.h"
#include "ast.h"
#include "transfrm.h"
#include "gramtk.h"
#include "extern.h"
#include "hpfutl.h"
#include "dinit.h"
#include "ccffinfo.h"
#include "optimize.h"
#include "rte.h"
#include "rtlRtns.h"

static void rewrite_into_forall(void);
static void rewrite_block_where(void);
static void rewrite_block_forall(void);
static void find_allocatable_assignment(void);
static void rewrite_allocatable_assignment(int astasgn, int std, LOGICAL);
static void gen_dealloc_if_allocated(int ast, int std);
static void trans_get_descrs(void);
static int trans_getidx(void);
static void trans_clridx(void);
static void trans_freeidx(void);
static int collapse_assignment(int, int);
static int build_sdsc_node(int);
static int _convert_int(int, int);
static int inline_spread_shifts(int, int, int);
static int copy_forall(int);
static void clear_dist_align(void);
static void transform_init(void);
static void declare_local_mode(void);
void init_finfo(void);
static void distribute_fval(void);
static int get_newdist_with_newproc(int dist);
static void set_initial_s1(void);
static LOGICAL contains_non0_scope(int astSrc);
static LOGICAL is_non0_scope(int sptr);
static void gen_allocated_check(int ast, int std, int atype, LOGICAL negate);
static int subscript_allocmem(int aref, int asd);
static int normalize_subscripts(int oldasd, int oldshape, int newshape);
static int gen_dos_over_shape(int shape, int std);
static void gen_do_ends(int docnt, int std);
static LOGICAL all_stride_one_shape(int shape);
static int mk_bounds_shape(int shape);
#if DEBUG
extern void dbg_print_stmts(FILE *);
#endif

FINFO_TBL finfot;
static int init_idx[MAXSUBS + MAXSUBS];
static int num_init_idx;
struct pure_gbl pure_gbl;

extern int pghpf_type_sptr;
int pghpf_local_mode_sptr = 0;

void
transform(void)
{
  pghpf_type_sptr = 0;
  pghpf_local_mode_sptr = 0;
  if (gbl.rutype != RU_BDATA) {
    transform_init();
    set_initial_s1();
    /* create descriptors */
    trans_get_descrs();

/* turn block wheres into single wheres */
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "Before rewrite_block_where\n");
      dstda();
    }
#endif
    rewrite_block_where();
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "After rewrite_block_where\n");
      dstda();
    }
#endif

    /* turn block foralls into single foralls */
    rewrite_block_forall();
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "After rewrite_block_forall\n");
      dstda();
    }
#endif

    /* transformational intrinsics */
    /* rewrite_forall_intrinsic();*/
    rewrite_forall_pure();
    if (flg.opt >= 2 && XBIT(53, 2)) {
      points_to();
    }
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "After rewrite_forall_pure\n");
      dstdpa();
    }
#endif

    /* Rewrite arguments to subroutines and uses of array-valued
     * functions */
    rewrite_calls();
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "After rewrite_calls\n");
      dstda();
    }
#endif

    find_allocatable_assignment();
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "After find_allocatable_assignment\n");
      dstda();
    }
#endif

    /* Transform array assignments, etc. into forall */
    rewrite_into_forall();
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "After rewrite_into_forall\n");
      dstda();
    }
#endif

    /* This routine rewrites those foralls
     * 1. forall with shape suc as A(i,:)
     * 2. forall with dependency,
     * 3. forall with distributed indirection array at rhs.
     */
    rewrite_forall();
#if DEBUG
    if (DBGBIT(50, 4)) {
      fprintf(gbl.dbgfil, "After rewrite_forall\n");
      dstda();
    }
#endif

#if DEBUG
    if (DBGBIT(50, 2)) {
      fprintf(gbl.dbgfil, "Statements after transform pass\n");
      dbg_print_stmts(gbl.dbgfil);
    }
#endif
    if (flg.opt >= 2 && XBIT(53, 2)) {
      f90_fini_pointsto();
    }

    trans_freeidx();

    if (sem.p_dealloc != 0) {
      interr("items were added to sem.p_dealloc but not freed", 0, ERR_Severe);
    }
  }
}

void
reset_init_idx(void)
{
  int i;
  for (i = 0; i < MAXSUBS + MAXSUBS; i++) {
    init_idx[i] = 0;
  }
}

static void
transform_init(void)
{
  int i;

  init_finfo();
  pure_gbl.local_mode = 0;
  pghpf_type_sptr = 0;
  pghpf_local_mode_sptr = 0;
  init_region();
  if (gbl.rutype != RU_BDATA) {
    for (i = 0; i < MAXSUBS + MAXSUBS; i++) {
      init_idx[i] = 0;
    }
    num_init_idx = 0;
  }
}

/*
 * set SDSDNS1 for descriptors of user array pointers or array-member pointers
 * for allocatables, assumed-shape, fixed-shape arrays, the associated
 * descriptors will always have a linear stride in the 1st dimension of one.
 * Also, set SDSCCONTIG for descriptors of user arrays with ALLOCATABLE
 * attribute, assumed-shape dummies, or fixed-shape arrays.
 */
static void
set_initial_s1(void)
{
  int sptr, sdsc, dtype, eldtype;
  for (sptr = stb.firstusym; sptr < stb.symavl; ++sptr) {
    switch (STYPEG(sptr)) {
    case ST_ARRAY:
    case ST_DESCRIPTOR:
    case ST_VAR:
    case ST_IDENT:
    case ST_STRUCT:
    case ST_MEMBER:
      if (IGNOREG(sptr))
        break;
      dtype = DTYPEG(sptr);
      if (dtype && DTY(dtype) == TY_ARRAY) {
        sdsc = SDSCG(sptr);
        if (sdsc != 0 && STYPEG(sdsc) != ST_PARAM) {
          /* an array with a section descriptor */
          if (!POINTERG(sptr)) {
            if ((SCG(sptr) == SC_DUMMY || SCG(sdsc) == SC_DUMMY) &&
                ASSUMSHPG(sptr)) {
              if (!XBIT(54, 2)) {
                /* don't set S1 for assumed-shape if -x 54 2 */
                SDSCS1P(sdsc, 1);
              }
            } else {
              SDSCS1P(sdsc, 1);
            }
          } else {
            /* set SDSCS1 for section descriptor if stride-1 */
            long s1;
            s1 = 0;
            if (s1) {
              SDSCS1P(sdsc, 1);
              SDSCCONTIGP(sdsc, 1);
              BYTELENP(sdsc, s1);
            }
          }
          if ((ALLOCATTRG(sptr) || (!XBIT(54, 2) && ASSUMSHPG(sptr))) &&
              !ASSUMLENG(sptr) && !ADJLENG(sptr) &&
              !(DDTG(DTYPEG(sptr)) == DT_DEFERCHAR ||
                DDTG(DTYPEG(sptr)) == DT_DEFERNCHAR)) {
            SDSCCONTIGP(sdsc, 1);
            eldtype = DTY(dtype + 1);
            BYTELENP(sdsc, size_of(eldtype));
          }
        }
        if (SCG(sptr) == SC_DUMMY) {
          sdsc = NEWDSCG(sptr);
          if (sdsc != 0 && STYPEG(sdsc) != ST_PARAM) {
            if (!POINTERG(sptr) && (!XBIT(54, 2) || !ASSUMSHPG(sptr))) {
              /* set SDSCS1 for section descriptor */
              /* don't set S1 for assumed-shape if -x 54 2 */
              SDSCS1P(sdsc, 1);
            }
            if ((ALLOCATTRG(sptr) || (!XBIT(54, 2) && ASSUMSHPG(sptr))) &&
                !ASSUMLENG(sptr) && !ADJLENG(sptr) &&
                !(DDTG(DTYPEG(sptr)) == DT_DEFERCHAR ||
                  DDTG(DTYPEG(sptr)) == DT_DEFERNCHAR)) {
              SDSCCONTIGP(sdsc, 1);
              eldtype = DTY(dtype + 1);
              BYTELENP(sdsc, size_of(eldtype));
            }
          }
        }
      }
      break;
    default:;
    }
  }
} /* set_initial_s1 */

int
get_init_idx(int i, int dtype)
{
  if (init_idx[i] == 0 || SCG(init_idx[i]) != symutl.sc) {
    char ci[2], cj[2];
    ci[0] = 'i';
    ci[1] = '\0';
    cj[0] = 'a' + num_init_idx;
    cj[1] = '\0';
    init_idx[i] = sym_get_scalar(ci, cj, dtype);
    ++num_init_idx;
    if (num_init_idx >= 26)
      num_init_idx = 0;
  }
  return init_idx[i];
} /* get_init_idx */

/* forall table */

void
init_finfo(void)
{
  finfot.size = 240;
  NEW(finfot.base, FINFO, finfot.size);
  finfot.avl = 1;
}

int
mk_finfo(void)
{
  int nd;

  nd = finfot.avl++;
  /*    finfot.avl += sizeof(FINFO); */
  NEED(finfot.avl, finfot.base, FINFO, finfot.size, finfot.size + 240);
  if (finfot.base == NULL)
    errfatal(7);
  return nd;
}

int
get_finfo(int forall, int a)
{
  int i;

  for (i = A_STARTG(forall); i > (int)(A_STARTG(forall) - A_NCOUNTG(forall));
       i--)
    if (a == FINFO_AST(i))
      return i;
  return 0;
}

int
get_finfo_type(int forall, int a, int type)
{
  int i;

  for (i = A_STARTG(forall); i > (int)(A_STARTG(forall) - A_NCOUNTG(forall));
       i--)
    if (a == FINFO_AST(i) && type == FINFO_TYPE(i))
      return i;
  return 0;
}

#define TRANS_AREA 10

static void
clear_dist_align(void)
{
  int sptr;
  int stype;

  for (sptr = stb.firstusym; sptr < stb.symavl; sptr++) {
    stype = STYPEG(sptr);
    if (stype == ST_ARRAY) {
      if (!ASSUMSHPG(sptr))
        SEQP(sptr, 1);
    }
  }
}

static struct {
  int sptr;
} wherestuff;

static void
nice_mask(int ast, LOGICAL *nice)
{
  switch (A_TYPEG(ast)) {
  case A_BINOP:
    if (A_OPTYPEG(ast) == OP_XTOX) /* real ** real */
      *nice = FALSE;
    break;
  case A_SUBSCR:
  case A_ID:
  case A_PAREN:
  case A_CONV:
  case A_CNST:
  case A_CMPLXC:
  case A_UNOP:
  case A_TRIPLE:
    break;
  default:
    *nice = FALSE;
    break;
  }
}

static LOGICAL
nice_where_mask(int ast)
{
  LOGICAL nice;

  nice = TRUE;
  ast_visit(1, 1);
  ast_traverse(ast, NULL, nice_mask, &nice);
  ast_unvisit();
  return nice;
}

static void
srch_sym(int ast, LOGICAL *has_sym)
{
  if (A_TYPEG(ast) == A_ID && wherestuff.sptr == A_SPTRG(ast))
    *has_sym = TRUE;
}

static LOGICAL
mask_on_lhs(int mask, int lhs)
{
  int sptr, stype;
  LOGICAL has_sym;

  /* find the LHS symbol */
  if (A_TYPEG(lhs) == A_SUBSCR)
    lhs = A_LOPG(lhs);
  if (A_TYPEG(lhs) != A_ID)
    return TRUE;
  sptr = A_SPTRG(lhs);
  stype = STYPEG(sptr);
  assert(stype == ST_ARRAY, "mask_on_lhs: sptr not array", sptr, 4);
  wherestuff.sptr = sptr;
  has_sym = FALSE;
  ast_visit(1, 1);
  ast_traverse(mask, NULL, srch_sym, &has_sym);
  ast_unvisit();
  return has_sym;
}

static void
rewrite_where_expr(int where_std, int endwhere_std)
{
  int ast, std;
  int astnew, stdnew;

  /* rewrite the where expression if it has transformationals, etc. */
  ast = STD_AST(where_std);
  /* If the expression requires a temporary as part of its
   * evaluation, must make sure that the temp is freed after
   * the WHERE, if it is a block where. An ugly way to
   * do this is to create a temp statement then move stuff
   * that gets added after it.
   */
  astnew = mk_stmt(A_CONTINUE, 0);
  stdnew = add_stmt_before(astnew, where_std);
  arg_gbl.std = stdnew;
  /*    A_IFEXPRP(ast, rewrite_sub_ast(A_IFEXPRG(ast)));*/
  /* all the stuff from between stdnew and where_std needs
   * to be moved after the ENDWHERE
   */
  if (STD_NEXT(stdnew) != where_std) {
    /* link the chain in after endwhere_std */
    STD_PREV(STD_NEXT(endwhere_std)) = STD_PREV(where_std);
    STD_NEXT(STD_PREV(where_std)) = STD_NEXT(endwhere_std);
    STD_NEXT(endwhere_std) = STD_NEXT(stdnew);
    STD_PREV(STD_NEXT(endwhere_std)) = endwhere_std;
    /* remove the chain after stdnew */
    STD_NEXT(stdnew) = where_std;
    STD_PREV(where_std) = stdnew;
  }
  /* unlink the dummy statement */
  STD_NEXT(STD_PREV(stdnew)) = STD_NEXT(stdnew);
  STD_PREV(STD_NEXT(stdnew)) = STD_PREV(stdnew);
  arg_gbl.std = where_std;
}

typedef struct wherestackentry {
  int where, elsewhere, forall;
} wherestackentry;

struct wherestack {
  wherestackentry *base;
  int size, topwhere, topforall;
} wherestack = {(wherestackentry *)0, 0, 0, 0};

/*
 * allocate the wherestack; also, initialize it at entry zero
 * with zero where/elsewhere statements
 */
static void
init_where(void)
{
  int top;
  wherestack.size = 5;
  NEW(wherestack.base, wherestackentry, wherestack.size);
  top = wherestack.topwhere = wherestack.topforall = 0;
  wherestack.base[top].where = 0;
  wherestack.base[top].elsewhere = 0;
  wherestack.base[top].forall = 0;
} /* init_where */

static void
push_where(int where_std)
{
  int top;
  ++wherestack.topwhere;
  NEED(wherestack.topwhere + 1, wherestack.base, wherestackentry,
       wherestack.size, 2 * wherestack.size);
  top = wherestack.topwhere;
  wherestack.base[top].where = where_std;
  wherestack.base[top].elsewhere = 0;
} /* push_where */

static void
push_elsewhere(int elsewhere_std)
{
  int top;
  top = wherestack.topwhere;
  if (top == 0)
    interr("rewrite_block_forall: elsewhere with no where", elsewhere_std, 3);
  if (wherestack.base[top].elsewhere != 0)
    interr("rewrite_block_forall: two elsewheres", elsewhere_std, 3);
  wherestack.base[top].elsewhere = elsewhere_std;
} /* push_elsewhere */

static void
pop_where(int *where, int *elsewhere)
{
  int top;
  top = wherestack.topwhere;
  if (top <= 0) {
    *where = 0;
    *elsewhere = 0;
  } else {
    *where = wherestack.base[top].where;
    *elsewhere = wherestack.base[top].elsewhere;
    --wherestack.topwhere;
  }
} /* pop_where */

static void
push_forall(int forall_std)
{
  int top;
  ++wherestack.topforall;
  NEED(wherestack.topforall + 1, wherestack.base, wherestackentry,
       wherestack.size, 2 * wherestack.size);
  top = wherestack.topforall;
  wherestack.base[top].forall = forall_std;
} /* push_forall */

static void
pop_forall(int *forall_std)
{
  int top;
  top = wherestack.topforall;
  if (top <= 0) {
    *forall_std = 0;
  } else {
    *forall_std = wherestack.base[top].forall;
    --wherestack.topforall;
  }
} /* pop_forall */

static void
add_wheresym(ITEM **wheresymlist, int wheresym)
{
  ITEM *itemp = (ITEM *)getitem(TRANS_AREA, sizeof(ITEM));
  itemp->next = *wheresymlist;
  itemp->t.sptr = wheresym;
  *wheresymlist = itemp;
}

static LOGICAL
in_wheresymlist(ITEM *list, int sptr)
{
  ITEM *itemp;
  for (itemp = list; itemp != ITEM_END; itemp = itemp->next) {
    if (itemp->t.sptr == sptr) {
      return TRUE;
    }
  }
  return FALSE;
}

/*
 * Transform block WHERE statements to single-statement wheres
 */
static void
rewrite_block_where(void)
{
  int std, stdnext, std1;
  int shape;
  int ast, ast1, ast2, lhs, nestedwhere;
  int where_load;
  int list;
  int wheresym;
  int sptr_lhs;
  int subscr[MAXSUBS];
  int where_std, elsewhere_std, endwhere_std;
  int outer_where_std, outer_endwhere_std;
  LOGICAL nice_where;
  int shape1;
  int parallel_depth;
  int task_depth;
  ITEM *wheresymlist = ITEM_END;

  init_where();

  /* Transform block wheres */
  where_std = elsewhere_std = 0;
  parallel_depth = 0;
  task_depth = 0;
  for (std = STD_NEXT(0); std != 0; std = stdnext) {
    stdnext = STD_NEXT(std);
    gbl.lineno = STD_LINENO(std);
    ast = STD_AST(std);
    switch (A_TYPEG(ast)) {
    case A_MP_PARALLEL:
      ++parallel_depth;
      /*symutl.sc = SC_PRIVATE;*/
      set_descriptor_sc(SC_PRIVATE);
      break;
    case A_MP_ENDPARALLEL:
      --parallel_depth;
      if (parallel_depth == 0 && task_depth == 0) {
        /*symutl.sc = SC_LOCAL;*/
        set_descriptor_sc(SC_LOCAL);
      }
      break;
    case A_MP_ETASKREG:
      if (parallel_depth == 0 && task_depth <= 1) {
        set_descriptor_sc(SC_LOCAL);
      }
      break;
    case A_MP_TASK:
      ++task_depth;
      break;
    case A_MP_ENDTASK:
      --task_depth;
      if (parallel_depth == 0 && task_depth == 0) {
        set_descriptor_sc(SC_LOCAL);
      }
      break;
    case A_FORALL:
      if (A_IFSTMTG(ast) == 0) {
        int astli, li;
        push_forall(std);
        /* mark the forall indices */
        astli = A_LISTG(ast);
        for (li = astli; li != 0; li = ASTLI_NEXT(li)) {
          int sptr = ASTLI_SPTR(li);
#if DEBUG
          if (FORALLNDXG(sptr)) {
            interr("rewrite_block_where: nested foralls with same index", std,
                   4);
          }
#endif
          FORALLNDXP(sptr, 1);
        }
      }
      break;
    case A_ENDFORALL: {
      int forall_std, forall_ast, astli, li;
      pop_forall(&forall_std);
      forall_ast = STD_AST(forall_std);
#if DEBUG
      if (A_TYPEG(forall_ast) != A_FORALL) {
        interr("rewrite_block_where: problem with endforall nesting", std, 4);
      }
#endif
      /* now unmark the forall indices */
      astli = A_LISTG(forall_ast);
      for (li = astli; li != 0; li = ASTLI_NEXT(li)) {
        int sptr = ASTLI_SPTR(li);
#if DEBUG
        if (!FORALLNDXG(sptr)) {
          interr("rewrite_block_where: forall index flag improperly reset", std,
                 4);
        }
#endif
        FORALLNDXP(sptr, 0);
      }
    } break;
    case A_WHERE:
      if (!A_IFSTMTG(ast)) {
        if (wherestack.topwhere == 0) {
          int std1, ast1, wherenest;
          /* this is the outermost WHERE, find outermost ENDWHERE */
          outer_where_std = std;
          outer_endwhere_std = 0;
          wherenest = 1;
          for (std1 = STD_NEXT(std); std1 > 0 && wherenest > 0;
               std1 = STD_NEXT(std1)) {
            ast1 = STD_AST(std1);
            switch (A_TYPEG(ast1)) {
            case A_WHERE:
              if (A_IFSTMTG(ast1) == 0)
                ++wherenest;
              break;
            case A_ENDWHERE:
              --wherenest;
              if (wherenest == 0)
                outer_endwhere_std = std1;
              break;
            }
          }
          if (outer_endwhere_std == 0)
            interr("rewrite_block_where: no outer endwhere", std, 4);
        }
        push_where(std);
      }
      break;
    case A_ELSEWHERE:
      assert(wherestack.topwhere > 0,
             "rewrite_block_where: ELSEWHERE with no WHERE", 0, 4);
      push_elsewhere(std);
      break;
    case A_ENDWHERE:
      /* end of block where. Try to optimize mask creation. If the
       * mask expression is 'nice', and no variable in the mask
       * expr is modified in the WHERE, then just use the expression
       * and its negation. Otherwise create a temp and use that.
       *
       * Use-def would be nice here, we'll hack it for now.
       */
      pop_where(&where_std, &elsewhere_std);
      endwhere_std = std;
      /* find lhs */
      lhs = 0;
      for (std1 = where_std; std1 != endwhere_std; std1 = STD_NEXT(std1)) {

        if (std1 == where_std || std1 == elsewhere_std)
          continue;

        ast = STD_AST(std1);
        /* might be a call or an allocate here,
         * front end rewrites array-valued
         * functions.
         */
        switch (A_TYPEG(ast)) {
        case A_CALL:
        case A_ALLOC:
        case A_CONTINUE:
        case A_COMMENT:
        case A_COMSTR:
        case A_DO:
        case A_ENDDO:
          continue;
        case A_WHERE:
          /* could be single-statement WHERE from nested where */
          ast = A_IFSTMTG(ast);
          break;
        case A_ASN:
          break;
        default:
          error(510, 4, STD_LINENO(where_std), CNULL, CNULL);
        }

        /* assignment node, look at lhs */
        lhs = A_DESTG(ast);
        if (HCCSYMG(memsym_of_ast(lhs))) {
          /* assignments to compiler generated symbols to not need
           * to be conformable  */
          continue;
        }
        shape = A_SHAPEG(lhs);
        if (shape == 0)
          continue;
        shape1 = A_SHAPEG(A_IFEXPRG(STD_AST(where_std)));
        if (!conform_shape(shape, shape1))
          error(511, 3, STD_LINENO(std), CNULL, CNULL);
        break;
      }
      if (!A_SHAPEG(A_IFEXPRG(STD_AST(where_std))))
        error(512, 4, STD_LINENO(where_std), CNULL, CNULL);
      rewrite_where_expr(where_std, endwhere_std);
      if (wherestack.topwhere > 0) {
        /* nested WHEREs always get temporary */
        nice_where = FALSE;
      } else {
        nice_where = nice_where_mask(A_IFEXPRG(STD_AST(where_std)));
      }

      where_load = A_IFEXPRG(STD_AST(where_std));
      for (std1 = where_std; nice_where && std1 != endwhere_std;
           std1 = STD_NEXT(std1)) {

        if (std1 == where_std || std1 == elsewhere_std)
          continue;

        ast = STD_AST(std1);
        /* might be a call or an allocate here,
         * front end rewrites array-valued
         * functions.
         */
        switch (A_TYPEG(ast)) {
        case A_CALL:
        case A_ALLOC:
        case A_CONTINUE:
        case A_COMMENT:
        case A_COMSTR:
        case A_DO:
        case A_ENDDO:
          continue;
        case A_WHERE:
          /* could be single-statement WHERE from nested where */
          ast = A_IFSTMTG(ast);
          break;
        case A_ASN:
          break;
        default:
          interr("rewrite_block_where: non assignment in WHERE", std1, 4);
        }

        /* assignment node, look at lhs */
        lhs = A_DESTG(ast);
        shape = A_SHAPEG(lhs);
        if (shape == 0)
          continue;
        /* this is an array assignment */
        if (mask_on_lhs(where_load, lhs))
          nice_where = FALSE;
      }
      if (!nice_where && lhs) {
        ast = STD_AST(where_std);
        shape = A_SHAPEG(A_IFEXPRG(ast));
        assert(shape != 0, "rewrite_block_where: bad where", std, 4);
        /* get a temp */
        assert(A_SHAPEG(lhs), "rewrite_block_where: no shape in WHERE", 0, 4);
        ast1 = lhs;
        if (ast1 == 0)
          ast1 = search_conform_array(A_IFEXPRG(ast), FALSE);
        if (ast1 == 0)
          ast1 = search_conform_array(A_IFEXPRG(ast), TRUE);
        assert(ast1 != 0, "rewrite_block_where: can't find array", 0, 4);
        wheresym = mk_assign_sptr(ast1, "ww", subscr, DT_LOG, &where_load);
        add_wheresym(&wheresymlist, wheresym);
      }
      for (std1 = where_std; std1 != endwhere_std; std1 = STD_NEXT(std1)) {

        if (std1 == where_std)
          continue;
        if (std1 == elsewhere_std) {
          if (nice_where)
            where_load = mk_unop(OP_LNOT, where_load, A_DTYPEG(where_load));
          continue;
        }
        ast = STD_AST(std1);

        nestedwhere = 0;
        switch (A_TYPEG(ast)) {
        case A_CALL:
        case A_ALLOC:
        case A_CONTINUE:
        case A_COMMENT:
        case A_COMSTR:
        case A_DO:
        case A_ENDDO:
          continue;
        case A_WHERE:
          /* could be single-statement WHERE from nested where */
          nestedwhere = A_IFEXPRG(ast);
          ast = A_IFSTMTG(ast);
          break;
        case A_ASN:
          break;
        default:
          interr("rewrite_block_where: non assignment in WHERE", std1, 4);
        }

        /* assignment node, look at lhs */
        lhs = A_DESTG(ast);

        sptr_lhs = memsym_of_ast(lhs);
        if (A_SHAPEG(A_DESTG(ast)) == 0 ||
            (HCCSYMG(sptr_lhs) && !in_wheresymlist(wheresymlist, sptr_lhs)))
          continue;

        /* this is an array assignment */

        /* make it a where */
        ast1 = mk_stmt(A_WHERE, 0);
        A_IFSTMTP(ast1, ast);
        if (nestedwhere) {
          /* make .AND. of condition; use SCAND as noncommutative AND */
          A_IFEXPRP(ast1, nestedwhere);
          nestedwhere =
              mk_binop(OP_SCAND, where_load, nestedwhere, A_DTYPEG(where_load));
        } else {
          A_IFEXPRP(ast1, where_load);
        }
        A_STDP(ast1, std1);
        STD_AST(std1) = ast1;
      }
      if (!nice_where && lhs) {
        /* make "wheresym = expr" */
        ast = STD_AST(where_std);
        ast2 = mk_stmt(A_ASN, DTYPEG(wheresym));
        A_DESTP(ast2, where_load);
        A_SRCP(ast2, A_IFEXPRG(ast));
        add_stmt_after(ast2, where_std);
        /* Insert the allocate statement */
        mk_mem_allocate(mk_id(wheresym), subscr, outer_where_std, 0);
        add_stmt_before(mk_assn_stmt(where_load, astb.i0, DT_LOG),
                        outer_where_std);

        if (elsewhere_std) {
          /* generate "where_sym = .not. where_sym" */
          ast2 = mk_unop(OP_LNOT, where_load, A_DTYPEG(where_load));
          ast1 = mk_stmt(A_ASN, DTYPEG(wheresym));
          A_DESTP(ast1, where_load);
          A_SRCP(ast1, ast2);
          add_stmt_after(ast1, elsewhere_std);
        }

        /* insert deallocate statement */
        mk_mem_deallocate(mk_id(wheresym), outer_endwhere_std);
      }
      if (where_std)
        ast_to_comment(STD_AST(where_std));
      if (elsewhere_std)
        ast_to_comment(STD_AST(elsewhere_std));
      if (endwhere_std)
        ast_to_comment(STD_AST(endwhere_std));
      break;
    default:
      break;
    }
  }
  FREE(wherestack.base);
}

static int ForallList;

/* This is the callback function for contains_forall_index(). */
static LOGICAL
_contains_forall_index(int ast, LOGICAL *flag)
{
  if (ast && A_TYPEG(ast) == A_ID) {
    int list;
    for (list = ForallList; list; list = ASTLI_NEXT(list)) {
      if (A_SPTRG(ast) == ASTLI_SPTR(list)) {
        *flag = TRUE;
        return TRUE;
      }
    }
  }
  return FALSE;
} /* _contains_forall_index */

/* Return TRUE if any index in the forall_list occurs somewhere within ast.
 * Modified from 'ast.c:contains_ast' */
static LOGICAL
contains_forall_index(int ast, int forall_list)
{
  LOGICAL result = FALSE;

  if (!ast)
    return FALSE;

  ForallList = forall_list;
  ast_visit(1, 1);
  ast_traverse(ast, _contains_forall_index, NULL, &result);
  ast_unvisit();
  return result;
} /* contains_forall_index */

static void
rewrite_block_forall(void)
{
  int std, stdnext, std1;
  int ast, ast1, ast2;
  int list, stmt;
  int expr, expr1, where_expr;
  int subscr[MAXSUBS];
  int forallb_std, endforall_std;
  int stack[MAXSUBS], top;
  int newforall;
  int forallb;

  /*
   * Transform block FORALL constructs to single-statement FORALLs
   */

  /* Transform block FORALLs */
  forallb_std = endforall_std = 0;
  top = 0;
  for (std = STD_NEXT(0); std != 0; std = stdnext) {
    stdnext = STD_NEXT(std);
    gbl.lineno = STD_LINENO(std);
    ast = STD_AST(std);
    if (A_TYPEG(ast) == A_FORALL && !A_IFSTMTG(ast)) {
      forallb_std = std;
      stack[top] = forallb_std;
      top++;
      assert(top <= MAXSUBS && top >= 0,
             "rewrite_block_forall: FORALL with no ENDFORALL", 0, 4);
    } else if (A_TYPEG(ast) == A_ENDFORALL) {
      endforall_std = std;
      top--;
      forallb_std = stack[top];
      assert(forallb_std, "rewrite_block_forall: FORALL with no ENDFORALL", 0,
             4);
      for (std1 = forallb_std; std1 != endforall_std; std1 = STD_NEXT(std1)) {

        gbl.lineno = STD_LINENO(std1);

        if (std1 == forallb_std) {
          forallb = STD_AST(forallb_std);
          continue;
        }

        ast = STD_AST(std1);
        /* might be a call or an allocate here,
         * front end rewrites array-valued
         * functions.
         */
        if (A_TYPEG(ast) == A_CALL) {
          if (!contains_forall_index(ast, A_LISTG(forallb)))
            continue;
        }
        if (A_TYPEG(ast) == A_ALLOC || A_TYPEG(ast) == A_CONTINUE ||
            A_TYPEG(ast) == A_COMMENT || A_TYPEG(ast) == A_COMSTR)
          continue;
        /* or it may be like, z_b_0 = 1 */
        if (A_TYPEG(ast) == A_ASN && A_TYPEG(A_DESTG(ast)) == A_ID)
          continue;

        switch (A_TYPEG(ast)) {
        case A_CALL:
        case A_ASN:
        case A_ICALL:
          expr = A_IFEXPRG(forallb);
          list = A_LISTG(forallb);
          stmt = ast;
          break;
        case A_WHERE:
          expr = A_IFEXPRG(forallb);
          where_expr = A_IFEXPRG(ast);
          if (expr)
            expr = mk_binop(OP_LAND, expr, where_expr, DT_LOG);
          else
            expr = where_expr;
          list = A_LISTG(forallb);
          stmt = A_IFSTMTG(ast);
          break;
        case A_FORALL:
          list = concatenate_list(A_LISTG(forallb), A_LISTG(ast));
          expr = A_IFEXPRG(forallb);
          expr1 = A_IFEXPRG(ast);
          if (expr && expr1)
            expr = mk_binop(OP_LAND, expr, expr1, DT_LOG);
          else if (expr1)
            expr = expr1;
          stmt = A_IFSTMTG(ast);
          break;
        default:
          interr("rewrite_block_forall: illegal statement in FORALL", ast, 3);
        }

        assert(stmt && list, "rewrite_block_forall: someting is wrong", ast, 4);
        newforall = mk_stmt(A_FORALL, 0);
        A_IFSTMTP(newforall, stmt);
        A_IFEXPRP(newforall, expr);
        A_LISTP(newforall, list);
        A_SRCP(newforall, A_SRCG(forallb));
        add_stmt_before(newforall, std1);
        ast_to_comment(STD_AST(std1));
      }
      ast_to_comment(STD_AST(forallb_std));
      ast_to_comment(STD_AST(endforall_std));
    }
  }
}

static void
check_subprogram(int std, int ast, int callast)
{
  int lop = A_LOPG(callast);
  int sptr = memsym_of_ast(lop);
  if (SEQUENTG(sptr)) { /* TPR 1786 */
                        /* go through the arguments;
                         * if any are array-valued, make forall */
    int shape, shapearg, i, cnt, argt, arg;
    shape = 0;
    cnt = A_ARGCNTG(callast);
    argt = A_ARGSG(callast);
    for (i = 0; i < cnt; ++i) {
      arg = ARGT_ARG(argt, i);
      if (arg > 0) {
        shape = A_SHAPEG(arg);
        shapearg = arg;
        if (shape)
          break;
      }
    }
    if (shape) { /* i is the argument with the shape */
      int ast1;
      ast1 = make_forall(shape, shapearg, 0, 0);
      for (i = 0; i < cnt; ++i) {
        arg = ARGT_ARG(argt, i);
        if (arg > 0) {
          arg = normalize_forall(ast1, arg, 0);
          ARGT_ARG(argt, i) = arg;
        }
      }
      A_IFSTMTP(ast1, ast);
      A_IFEXPRP(ast1, 0);
      A_STDP(ast1, std);
      STD_AST(std) = ast1;
    }
  }
} /* check_subprogram */

static void
rewrite_into_forall(void)
{
  int std, stdnext;
  int shape;
  int ast, ast1, ast2, lhs, rhs;
  int where_load;
  int list;
  int wheresym;
  int sptr;
  int shape1, shape2;
  int parallel_depth;
  int task_depth;
  int copy_ast = 0, dealloc_ast = 0;

  /*
   * Transform WHERE statements to foralls, and transform block-forall
   * statements to single-statement foralls.
   *
   * Block-foralls can be left alone when back end is prepared to handle
   * them.
   *
   * Subset HPF doesn't allow block foralls.
   *
   * IF statements are transformed to IF-THEN-ENDIF statements so that
   * communication calls can be inserted without trouble.
   *
   * Some call statements are inspected and elementalized if they
   * have array arguments (specifically, F90 IO routines).
   */

  parallel_depth = 0;
  task_depth = 0;
  for (std = STD_NEXT(0); std; std = stdnext) {
    stdnext = STD_NEXT(std);
    gbl.lineno = STD_LINENO(std);
    ast = STD_AST(std);
    switch (A_TYPEG(ast)) {
    case A_WHERE:
      if (A_IFSTMTG(ast)) {
        if (!A_SHAPEG(A_IFEXPRG(ast)))
          error(512, 4, STD_LINENO(std), CNULL, CNULL);
        shape1 = A_SHAPEG(A_IFEXPRG(ast));
        shape2 = A_SHAPEG(A_DESTG(A_IFSTMTG(ast)));
        if (!conform_shape(shape1, shape2))
          error(511, 3, STD_LINENO(std), CNULL, CNULL);
        /* single-stmt where */
        /* create forall stmt */
        /* forall is normalized with respect to the LHS expression */
        ast1 = make_forall(A_SHAPEG(A_DESTG(A_IFSTMTG(ast))),
                           A_DESTG(A_IFSTMTG(ast)), A_IFEXPRG(ast), 0);
        /* flag to show that it is made from arrray assignment */
        A_ARRASNP(ast1, 1);

        ast2 = normalize_forall(ast1, A_IFSTMTG(ast), 0);
        /* replace this ast with forall */
        A_IFSTMTP(ast1, ast2);
        A_STDP(ast1, std);
        STD_AST(std) = ast1;
      } else {
        interr("rewrite_info_forall: WHERE construct", std, 4);
      }
      break;
    case A_ELSEWHERE:
    case A_ENDWHERE:
      interr("rewrite_info_forall: WHERE construct", std, 4);
      break;
    case A_ASN:
      /* assignment node, look at lhs */
      lhs = A_DESTG(ast);
      rhs = A_SRCG(ast);

      /* if it is string, don't touch it */
      if (A_TYPEG(lhs) == A_SUBSTR && A_TYPEG(A_LOPG(lhs)) == A_SUBSCR)
        lhs = A_LOPG(lhs);

      shape = A_SHAPEG(lhs);
      if (shape) {
/*
 * check if array assignment can be collapsed into a single
 * memset/move
 */
          ast1 = collapse_assignment(ast, std);
        if (ast1) {
          std = add_stmt_after(ast1, std);
          ast_to_comment(ast);
        } else {
          /* this is an array assignment; need to create a forall */
          ast1 = make_forall(shape, lhs, 0, 0);
          ast2 = normalize_forall(ast1, ast, 0);
          A_IFSTMTP(ast1, ast2);
          A_IFEXPRP(ast1, 0);
          A_STDP(ast1, std);
          STD_AST(std) = ast1;
          /* flag to show that it is made from array assignment */
          A_ARRASNP(ast1, 1);
          STD_ZTRIP(std) = 1;
        }
      } else {
        if (A_TYPEG(rhs) == A_FUNC) {
          check_subprogram(std, ast, rhs);
        }
      }
      break;
    case A_CALL:
      check_subprogram(std, ast, ast);
      break;
    case A_MP_PARALLEL:
      ++parallel_depth;
      /*symutl.sc = SC_PRIVATE;*/
      set_descriptor_sc(SC_PRIVATE);
      break;
    case A_MP_ENDPARALLEL:
      --parallel_depth;
      if (parallel_depth == 0 && task_depth == 0) {
        /*symutl.sc = SC_LOCAL;*/
        set_descriptor_sc(SC_LOCAL);
      }
      break;
    case A_MP_TASKREG:
      set_descriptor_sc(SC_PRIVATE);
      break;
    case A_MP_ETASKREG:
      if (parallel_depth == 0 && task_depth <= 1) {
        set_descriptor_sc(SC_LOCAL);
      }
      break;
    case A_MP_TASK:
      ++task_depth;
      break;
    case A_MP_ENDTASK:
      --task_depth;
      if (parallel_depth == 0 && task_depth == 0) {
        set_descriptor_sc(SC_LOCAL);
      }
      break;
    default:
      break;
    }
  }
}

int
search_arr(int ast)
{
  int ast1;

  if (A_TYPEG(ast) == A_SUBSCR)
    ast = A_LOPG(ast);
  /*    assert(A_TYPEG(ast) == A_ID, "search_arr: not ID", ast, 4); */
  assert(DTY(A_DTYPEG(ast)) == TY_ARRAY, "search_arr: not TY_ARRAY", ast, 4);
  return ast;
}

/* Convert ast from an index with oldlb and oldstride to one with
 * newlb and newstride.  I.e.
 *   (ast - oldlb) / oldstride * newstride + newlb
 */
static int
normalize_subscript(int ast, int oldlb, int oldstride, int newlb, int newstride)
{
  if (oldstride == 0)
    oldstride = astb.bnd.one;
  if (newstride == 0)
    newstride = astb.bnd.one;
  if (oldstride == newstride) {
    if (oldlb != newlb) {
      ast = mk_binop(OP_SUB, ast, oldlb, astb.bnd.dtype);
      ast = mk_binop(OP_ADD, ast, newlb, astb.bnd.dtype);
    }
  } else {
    if (oldstride == mk_isz_cval(-1, astb.bnd.dtype)) {
      ast = mk_binop(OP_SUB, oldlb, ast, astb.bnd.dtype);
    } else {
      ast = mk_binop(OP_SUB, ast, oldlb, astb.bnd.dtype);
      ast = mk_binop(OP_DIV, ast, oldstride, astb.bnd.dtype);
    }
    ast = mk_binop(OP_MUL, ast, newstride, astb.bnd.dtype);
    ast = mk_binop(OP_ADD, ast, newlb, astb.bnd.dtype);
  }
  return ast;
}

/** \brief Return TRUE if memast is an A_MEM for an array, or
    memast is an A_SUBSCR whose parent is an A_MEM and which
    has triplet subscripts */
LOGICAL
vector_member(int memast)
{
  if (A_TYPEG(memast) == A_MEM) {
    int sptr = A_SPTRG(A_MEMG(memast));
    if (DTY(DTYPEG(sptr)) == TY_ARRAY)
      return TRUE;
    return FALSE;
  }
  if (A_TYPEG(memast) == A_SUBSCR) {
    int asd, i, n;
    asd = A_ASDG(memast);
    n = ASD_NDIM(asd);
    for (i = 0; i < n; ++i) {
      int ss = ASD_SUBS(asd, i);
      if (A_SHAPEG(ss) > 0)
        return TRUE;
      if (A_TYPEG(ss) == A_TRIPLE)
        return TRUE;
    }
  }
  return FALSE;
} /* vector_member */

static int
normalize_forall_array(int forall_ast, int arr_ast, int inlist)
{
  int i, j, triple;
  int list;
  int shape, vectmem;
  int ast;
  int ast1;
  int asd;
  int subs[MAXSUBS];
  int numdim;
  int l;
  int lwb, stride;
  LOGICAL flag;

  /* arr_ast is an array subscript or a whole array reference.
   * Normalize the indices into arr_ast
   */
  shape = A_SHAPEG(arr_ast);
  assert(shape != 0, "normalize_forall_array: 0 shape", arr_ast, 4);
  if (A_TYPEG(arr_ast) == A_ID || A_TYPEG(arr_ast) == A_MEM) {
    asd = 0;
    numdim = SHD_NDIM(shape);
  } else if (A_TYPEG(arr_ast) == A_SUBSCR) {
    asd = A_ASDG(arr_ast);
    numdim = ASD_NDIM(asd);
    j = SHD_NDIM(shape);
  } else {
    interr("normalize_forall_array:bad ast type", arr_ast, 3);
  }

  if (numdim < 1 || numdim > MAXSUBS) {
    interr("normalize_forall_array:bad numdim", shape, 3);
    numdim = 0;
  }

  /* do this call now, instead of later, because arr_ast may
   * be changed in place */
  vectmem = vector_member(arr_ast);
  if (inlist != 0) {
    /* this is a vector subscript. Use the ast list that was passed in */
    list = inlist;
  } else {
    list = A_LISTG(forall_ast);
  }
  for (i = numdim - 1; i >= 0; i--) {
    flag = FALSE;
    if (asd) {
      if (A_TYPEG(ASD_SUBS(asd, i)) == A_TRIPLE) {
        assert(j > 0, "normalize_forall_array: SHD/ASD mismatch", forall_ast,
               4);
        --j;
        lwb = SHD_LWB(shape, j);
        stride = SHD_STRIDE(shape, j);
        flag = TRUE;
      } else if (A_SHAPEG(ASD_SUBS(asd, i))) {
        /* vector subscript */
        lwb = normalize_forall(forall_ast, ASD_SUBS(asd, i), list);
        flag = FALSE;
        list = ASTLI_NEXT(list);
        --j;
      } else {
        /* scalar subscript */
        lwb = ASD_SUBS(asd, i);
        flag = FALSE;
      }
    } else {
      lwb = check_member(arr_ast, SHD_LWB(shape, i));
      stride = check_member(arr_ast, SHD_STRIDE(shape, i));
      flag = TRUE;
    }

    if (flag) {
      int sptr = ASTLI_SPTR(list);
      assert(list != 0, "normalize_forall_array: non-conformable", arr_ast, 4);
      triple = ASTLI_TRIPLE(list);
      if (sptr == 0) {
        subs[i] = triple;
      } else {
        subs[i] = normalize_subscript(mk_id(sptr), A_LBDG(triple),
                                      A_STRIDEG(triple), lwb, stride);
      }
      list = ASTLI_NEXT(list);
    } else {
      subs[i] = lwb;
    }
  }

  ast = search_arr(arr_ast);
  if (vectmem) {
    /* This is a%b(:), where a and b are both arrays. We want
     * a%b(i)
     */
    ast = mk_subscr(ast, subs, numdim, DDTG(A_DTYPEG(arr_ast)));
  } else if (A_TYPEG(ast) == A_MEM) {
    /* This is a%b(i), where a and b are both arrays. We want
     * a(j)%b(i)
     */
    int ast1;
    int subs1[MAXSUBS];
    int n1;
    ast1 =
        mk_subscr(A_PARENTG(ast), subs, numdim, DDTG(A_DTYPEG(A_PARENTG(ast))));
    ast = mk_member(ast1, A_MEMG(ast), DDTG(A_DTYPEG(A_MEMG(ast))));
    if (A_TYPEG(arr_ast) == A_SUBSCR) {
      asd = A_ASDG(arr_ast);
      n1 = ASD_NDIM(asd);
      for (i = 0; i < n1; ++i)
        subs1[i] = ASD_SUBS(asd, i);
      ast = mk_subscr(ast, subs1, n1, DDTG(A_DTYPEG(A_MEMG(ast))));
    } else
      ast = mk_subscr(ast, subs, numdim, DDTG(A_DTYPEG(arr_ast)));
  } else
    ast = mk_subscr(ast, subs, numdim, DDTG(A_DTYPEG(arr_ast)));
  return ast;
}

static int
normalize_id(int forall_ast, int asgn_ast, int inlist)
{
  int org_shape, newast, nd, nc;
  org_shape = A_SHAPEG(asgn_ast);
  newast = normalize_forall_array(forall_ast, asgn_ast, inlist);
  /*            A_SECSHPP(newast, org_shape); */ /* keep original shape */
  /* put info into FINFO table */
  nd = mk_finfo();
  FINFO_AST(nd) = newast;
  FINFO_SHAPE(nd) = org_shape;
  FINFO_TYPE(nd) = 0;
  A_STARTP(forall_ast, nd);
  nc = A_NCOUNTG(forall_ast) + 1;
  A_NCOUNTP(forall_ast, nc);
  return newast;
} /* normalize_id */

int
normalize_forall(int forall_ast, int asgn_ast, int inlist)
{
  /* forall_ast represents a forall statement with one or more indices.
   * asgn_ast represents an array assignment with or without triple
   * expressions.  Create a new ast, replacing the triples or whole-array
   * dimensions of the asgn_ast with indices representing the same
   * sections, expressed as functions of the forall_ast index variables */
  int ast, ast1, ast2;
  int dtype;
  int argt, nargs, i;
  int newast, org_shape;
  int nd, nc;
  int shape;

  if (asgn_ast == 0)
    return 0;
  switch (A_TYPEG(asgn_ast)) {
  case A_ASN:
    ast1 = normalize_forall(forall_ast, A_DESTG(asgn_ast), inlist);
    ast2 = normalize_forall(forall_ast, A_SRCG(asgn_ast), inlist);
    ast = mk_stmt(A_ASN, A_DTYPEG(ast1));
    A_DESTP(ast, ast1);
    A_SRCP(ast, ast2);
    return ast;
  case A_BINOP:
    ast1 = normalize_forall(forall_ast, A_LOPG(asgn_ast), inlist);
    ast2 = normalize_forall(forall_ast, A_ROPG(asgn_ast), inlist);
    dtype = A_DTYPEG(asgn_ast);
    if (DTY(dtype) == TY_ARRAY)
      dtype = DTY(dtype + 1);
    return mk_binop(A_OPTYPEG(asgn_ast), ast1, ast2, dtype);
  case A_UNOP:
    ast1 = normalize_forall(forall_ast, A_LOPG(asgn_ast), inlist);
    dtype = A_DTYPEG(asgn_ast);
    if (DTY(dtype) == TY_ARRAY)
      dtype = DTY(dtype + 1);
    return mk_unop(A_OPTYPEG(asgn_ast), ast1, dtype);
  case A_CONV:
    ast1 = normalize_forall(forall_ast, A_LOPG(asgn_ast), inlist);
    dtype = A_DTYPEG(asgn_ast);
    if (DTY(dtype) == TY_ARRAY)
      dtype = DTY(dtype + 1);
    if (is_iso_cptr(dtype) && A_OPTYPEG(A_LOPG(asgn_ast))) {
      A_DTYPEP(ast1, DT_PTR);
      dtype = DT_PTR;
    }
    return mk_convert(ast1, dtype);
  case A_CMPLXC:
  case A_CNST:
    return asgn_ast;
  case A_SUBSTR:
    ast = normalize_forall(forall_ast, A_LOPG(asgn_ast), inlist);
    return mk_substr(ast, A_LEFTG(asgn_ast), A_RIGHTG(asgn_ast),
                     A_DTYPEG(asgn_ast));
  case A_PAREN:
    ast = normalize_forall(forall_ast, A_LOPG(asgn_ast), inlist);
    return mk_paren(ast, A_DTYPEG(ast));

  case A_INTR:
    return inline_spread_shifts(asgn_ast, forall_ast, inlist);
  case A_FUNC:
    shape = A_SHAPEG(asgn_ast);
    if (shape) {
      argt = A_ARGSG(asgn_ast);
      nargs = A_ARGCNTG(asgn_ast);
      for (i = 0; i < nargs; ++i) {
        ARGT_ARG(argt, i) =
            normalize_forall(forall_ast, ARGT_ARG(argt, i), inlist);
      }
      dtype = A_DTYPEG(asgn_ast);
      if (DTY(dtype) == TY_ARRAY && elemental_func_call(asgn_ast)) {
        A_DTYPEP(asgn_ast, DTY(dtype + 1));
        A_SHAPEP(asgn_ast, 0);
      }
    }
    return asgn_ast;
  case A_SUBSCR:
    /* does this subscript have any triplet entries */
    if (vector_member(asgn_ast)) {
      asgn_ast = normalize_id(forall_ast, asgn_ast, inlist);
    }
    if (A_TYPEG(A_LOPG(asgn_ast)) == A_MEM) {
      /* the parent might have an array index */
      int asd, i, n, subs[MAXSUBS], dtype;
      asd = A_ASDG(asgn_ast);
      ast = normalize_forall(forall_ast, A_PARENTG(A_LOPG(asgn_ast)), inlist);
      if (ast != A_PARENTG(A_LOPG(asgn_ast))) {
        dtype = A_DTYPEG(A_MEMG(A_LOPG(asgn_ast)));
        ast = mk_member(ast, A_MEMG(A_LOPG(asgn_ast)), dtype);
        if (DTY(dtype) == TY_ARRAY)
          dtype = DTY(dtype + 1);
        /* add the member subscripts */
        n = ASD_NDIM(asd);
        for (i = 0; i < n; ++i) {
          subs[i] = ASD_SUBS(asd, i);
        }
        asgn_ast = mk_subscr(ast, subs, n, dtype);
      }
    }
    return asgn_ast;
  case A_MEM:
    if (vector_member(asgn_ast)) {
      return normalize_id(forall_ast, asgn_ast, inlist);
    } else {
      /* the parent might have an array index */
      ast = normalize_forall(forall_ast, A_PARENTG(asgn_ast), inlist);
      /* member should be a scalar here */
      return mk_member(ast, A_MEMG(asgn_ast), A_DTYPEG(A_MEMG(asgn_ast)));
    }
  case A_ID:
    if (DTY(A_DTYPEG(asgn_ast)) == TY_ARRAY) {
      return normalize_id(forall_ast, asgn_ast, inlist);
    }
    return asgn_ast;
  default:
    interr("normalize_forall: bad opc", asgn_ast, 3);
    return asgn_ast;
  }
}

/*
 * check if array assignment can be collapsed into a single memset/move
 */
static int
collapse_assignment(int asn, int std)
{
  int lhs, rhs;
  int rhs_allocatable;
  int shape;
  int ast;
  int cnst;
  int dtype;
  int dest;
  int src;
  int ndim;
  int i;
  int func;
  int sz;
  int szdtype;
  int one;
  int is_zero;
  int use_numelm;
  char *nm;
  FtnRtlEnum rtlRtn;
  int rhs_isptr, lhs_isptr;

  if (flg.opt < 2)
    return 0;

  if (XBIT(8, 0x8000000))
    return 0;

  rhs_isptr = 0;
  lhs_isptr = 0;
  lhs = A_DESTG(asn);
  shape = A_SHAPEG(lhs);
  ndim = SHD_NDIM(shape);
  if (XBIT(34, 0x200) && ndim > 2) {
    /*
     * assume -Mconcur is better than collapsing an assignment of 3D
     * or greater array.  For a >= 3D array:
     * +  the backend replaces the innermost loop with an idiom, and
     *    the idiom is now part of the next loop;
     * +  autopar does not parallelize the loop containing the idiom;
     * +  autopar parallelizes the outer (originally the 3rd) loop.
     */
    return 0;
  }
  /*
   * look at the rhs of the assignment; for now, limit it to a
   * constant, scalar, array, contiguous array section of a basic
   * numeric type.
   */
  rhs_allocatable = 0;
  src = 0;
  rhs = A_SRCG(asn);
  dtype = A_DTYPEG(rhs);
  switch (A_TYPEG(rhs)) {
  case A_CONV:
    src = 0;
    break;
  case A_ID:
    /* can only be rank 1 if assumed-shape */
    src = A_SPTRG(rhs);
    if (SCG(src) == SC_DUMMY && ASSUMSHPG(src) && ndim > 1 && !CONTIGATTRG(src))
      return 0;
    goto rhs_chk;
  case A_MEM:
    /*  member must be array instead of some parent */
    src = A_SPTRG(A_MEMG(rhs));
    if (DTY(DTYPEG(src)) != TY_ARRAY)
      return 0;
  rhs_chk:
    if (POINTERG(src)) {
      rhs_isptr = 1;
    }
    if (ALLOCATTRG(src)) {
      rhs_allocatable = 1;
    }
    break;
  case A_SUBSCR:
    if (!contiguous_section(rhs))
      return 0;
    src = find_array(rhs, NULL);
    if (STYPEG(src) != ST_MEMBER && SCG(src) == SC_DUMMY && ASSUMSHPG(src) &&
        ndim > 1)
      return 0;
    if (POINTERG(src)) {
      rhs_isptr = 1;
    }
    rhs = first_element(rhs);
    break;
  default:
    return 0;
  }

  if (!src) {
    /*  WANT scalar rhs */
    rhs = A_LOPG(rhs);
    /* check for scalar to a array conversion */
    if (DTY(A_DTYPEG(rhs)) == TY_ARRAY)
      return 0;
  }
  dtype = DDTG(dtype);
  if (!DT_ISNUMERIC(dtype) && !DT_ISLOG(dtype))
    return 0;
  cnst = 0;
  if (A_TYPEG(rhs) == A_CNST)
    /* scalar constant */
    cnst = A_SPTRG(A_ALIASG(rhs));

  /* look at the lhs of the assignment */
  use_numelm = 1;
  if (A_TYPEG(lhs) == A_ID) {
    /* can only be rank 1 if assumed-shape */
    dest = A_SPTRG(lhs);
    if (SCG(dest) == SC_DUMMY && ASSUMSHPG(dest)) {
      use_numelm = 0;
      if (ndim > 1 && !CONTIGATTRG(dest))
        return 0;
    }
  } else if (A_TYPEG(lhs) == A_MEM) {
    dest = A_SPTRG(A_MEMG(lhs));
    /*  member must be array instead of some parent */
    if (DTY(DTYPEG(dest)) != TY_ARRAY)
      return 0;
  } else {
    use_numelm = 0; /* section??? */
    return 0;
  }
  if (POINTERG(dest)) {
    use_numelm = 0;
    lhs_isptr = 1;
  }
  if ((ADD_NUMELM(DTYPEG(dest))) == 0) {
    use_numelm = 0;
  }
  if (ndim <= 1 && !DT_ISCMPLX(dtype) && !ASSUMSHPG(dest))
    return 0;
  if (ALLOCATTRG(dest)) {
    if (src && rhs_allocatable && XBIT(54, 0x1))
      /* allocatable <- allocatable & f2003 semantics */
      return 0;
    use_numelm = 0;
  } else if (ALLOCG(dest))
    use_numelm = 0;

  /***********************************************************
   * scn (03 Oct 2014): -0.0 is not considered to be 0.0 here
   ***********************************************************/
  is_zero = 0;
  if (cnst) {
    switch (dtype) {
    case DT_CMPLX8:
      if (CONVAL1G(cnst) == 0 && CONVAL2G(cnst) == 0)
        is_zero = 1;
      break;
    case DT_CMPLX16:
      if (CONVAL1G(cnst) == stb.dbl0 && CONVAL2G(cnst) == stb.dbl0)
        is_zero = 1;
      break;
    case DT_BINT:
    case DT_SINT:
    case DT_INT4:
    case DT_BLOG:
    case DT_SLOG:
    case DT_LOG4:
      if (CONVAL2G(cnst) == 0)
        is_zero = 1;
      break;
    case DT_LOG8:
      if (CONVAL1G(cnst) == 0 && CONVAL2G(cnst) == 0)
        is_zero = 1;
      break;
    default:
      if (cnst == stb.i0 || cnst == stb.k0 || cnst == stb.flt0 ||
          cnst == stb.dbl0)
        is_zero = 1;
      break;
    }
  }

  szdtype = DT_INT8;
  sz = one = astb.k1;

  if (lhs_isptr || rhs_isptr) {
    if (lhs_isptr && rhs_isptr) { /* could have an overlap */
      /*** do work in progress ***/
      return 0;
    }
    if (lhs_isptr && !CONTIGATTRG(dest))
      return 0;
    if (rhs_isptr && !CONTIGATTRG(src))
      return 0;
  }

  if (use_numelm) {
#if DEBUG
    if (ADD_NUMELM(DTYPEG(dest)) == 0)
      error(0, 2, gbl.lineno, "ADD_NUMELM(DTYPEG(dest) is 0 ", CNULL);
#endif
    sz = _convert_int(ADD_NUMELM(DTYPEG(dest)), szdtype);
  } else {
    /* compute size from shape descriptor */
    for (i = ndim - 1; i >= 0; i--) {
      int lwb, upb, aa;
      lwb = check_member(lhs, SHD_LWB(shape, i));
      lwb = _convert_int(lwb, szdtype);
      upb = check_member(lhs, SHD_UPB(shape, i));
      upb = _convert_int(upb, szdtype);
      aa = mk_binop(OP_SUB, upb, lwb, szdtype);
      aa = mk_binop(OP_ADD, aa, one, szdtype);
      sz = mk_binop(OP_MUL, sz, aa, szdtype);
    }
  }
  if (is_zero) {
    if (DT_ISCMPLX(dtype)) {
      switch (size_of(dtype)) {
      case 8:
        rtlRtn = RTE_mzeroz8;
        break;
      case 16:
        rtlRtn = RTE_mzeroz16;
        break;
      }
    } else {
      switch (size_of(dtype)) {
      case 1:
        rtlRtn = RTE_mzero1;
        break;
      case 2:
        rtlRtn = RTE_mzero2;
        break;
      case 4:
        rtlRtn = RTE_mzero4;
        break;
      case 8:
        rtlRtn = RTE_mzero8;
        break;
      }
    }
    nm = mkRteRtnNm(rtlRtn);
    func = sym_mkfunc_nodesc(nm, DT_INT);
    ast = begin_call(A_CALL, func, 2);
    add_arg(lhs);
    /*add_arg(sz);*/
    add_arg(mk_unop(OP_VAL, sz, szdtype));
    ccff_info(MSGOPT, "OPT008", gbl.findex, gbl.lineno,
              "Memory zero idiom, array assignment replaced by call to %mzero",
              "mzero=%s", nm, NULL);
  } else if (src) {
    if (DT_ISCMPLX(dtype)) {
      switch (size_of(dtype)) {
      case 8:
        rtlRtn = RTE_mcopyz8;
        break;
      case 16:
        rtlRtn = RTE_mcopyz16;
        break;
      }
    } else {
      switch (size_of(dtype)) {
      case 1:
        rtlRtn = RTE_mcopy1;
        break;
      case 2:
        rtlRtn = RTE_mcopy2;
        break;
      case 4:
        rtlRtn = RTE_mcopy4;
        break;
      case 8:
        rtlRtn = RTE_mcopy8;
        break;
      }
    }
    nm = mkRteRtnNm(rtlRtn);
    func = sym_mkfunc_nodesc(nm, DT_INT);
    ast = begin_call(A_CALL, func, 3);
    add_arg(lhs);
    add_arg(rhs);
    /*add_arg(sz);*/
    add_arg(mk_unop(OP_VAL, sz, szdtype));
    ccff_info(MSGOPT, "OPT006", gbl.findex, gbl.lineno,
              "Memory copy idiom, array assignment replaced by call to %mcopy",
              "mcopy=%s", nm, NULL);
  } else {
    if (DT_ISCMPLX(dtype)) {
      switch (size_of(dtype)) {
      case 8:
        rtlRtn = RTE_msetz8;
        break;
      case 16:
        rtlRtn = RTE_msetz16;
        break;
      }
    } else {
      switch (size_of(dtype)) {
      case 1:
        rtlRtn = RTE_mset1;
        break;
      case 2:
        rtlRtn = RTE_mset2;
        break;
      case 4:
        rtlRtn = RTE_mset4;
        break;
      case 8:
        rtlRtn = RTE_mset8;
        break;
      }
    }
    nm = mkRteRtnNm(rtlRtn);
    func = sym_mkfunc_nodesc(nm, DT_INT);
    ast = begin_call(A_CALL, func, 3);
    add_arg(lhs);
    add_arg(rhs);
    /*add_arg(sz);*/
    add_arg(mk_unop(OP_VAL, sz, szdtype));
    ccff_info(MSGOPT, "OPT007", gbl.findex, gbl.lineno,
              "Memory set idiom, array assignment replaced by call to %mset",
              "mset=%s", nm, NULL);
  }
  /*dbg_print_ast(ast, STDERR);*/
  return ast;
}

static int
_convert_int(int ast, int dtype)
{
  if (A_DTYPEG(ast) == dtype)
    return ast;
  ast = mk_convert(ast, dtype);
  return ast;
}

static int
inline_spread_shifts(int asgn_ast, int forall_ast, int inlist)
{
  int argt, nargs;
  int list, listp, astli;
  int newlist;
  int count, nidx;
  int subs[MAXSUBS];
  int ndim;
  int dim, cdim, shd;
  int srcarray, maskarray;
  int newforall;
  int i, j;
  int asd;
  int retval, newast;
  int shift, cshift;
  int nd;
  int func_ast;
  int dtype;
  int boundary;

  assert(A_TYPEG(asgn_ast) == A_INTR, "inline_spread_shifts: wrong ast type",
         asgn_ast, 3);
  if (INKINDG(A_SPTRG(A_LOPG(asgn_ast))) == IK_INQUIRY)
    return asgn_ast;
  argt = A_ARGSG(asgn_ast);
  nargs = A_ARGCNTG(asgn_ast);
  switch (A_OPTYPEG(asgn_ast)) {
  case I_SPREAD: /* spread(source, dim, ncopies) */
    srcarray = ARGT_ARG(argt, 0);
    dim = ARGT_ARG(argt, 1);
    if (!A_SHAPEG(srcarray))
      dim = astb.i1;
    if (A_TYPEG(dim) != A_CNST)
      goto ret_norm;
    cdim = get_int_cval(A_SPTRG(dim));
    newforall = copy_forall(forall_ast);
    list = A_LISTG(newforall);
    nidx = 1;
    for (listp = list; listp != 0; listp = ASTLI_NEXT(listp))
      nidx++;
    count = 1;
    astli = 0;
    for (listp = list; listp != 0; listp = ASTLI_NEXT(listp)) {
      if (count == nidx - cdim)
        astli = listp;
      count++;
    }
    assert(astli, "normalize_forall: something is wrong", astli, 3);
    list = delete_astli(list, astli);
    A_LISTP(newforall, list);
    newast = normalize_forall(newforall, srcarray, inlist);
    return newast;

  case I_TRANSPOSE: /* transpose(matrix) */
    srcarray = ARGT_ARG(argt, 0);
    /* transpose the forall index */
    newforall = copy_forall(forall_ast);
    list = A_LISTG(newforall);
    count = 0;
    for (listp = list; listp != 0; listp = ASTLI_NEXT(listp)) {
      subs[count] = listp;
      count++;
      assert(count <= MAXSUBS, "inline_spread_shifts: wrong  forall", newforall,
             4);
    }

    /* only transpose the first two indices;
     * if there are more than two, we assume (hopefully) that
     * the others come from the indices added to handle
     * componentized array members of derived types */
    start_astli();
    if (count < 2) {
      listp = subs[0];
      newlist = add_astli();
      ASTLI_SPTR(newlist) = ASTLI_SPTR(listp);
      ASTLI_TRIPLE(newlist) = ASTLI_TRIPLE(listp);
    } else {
      /* switch 1 and 0 */
      for (i = 1; i >= 0; --i) {
        listp = subs[i];
        newlist = add_astli();
        ASTLI_SPTR(newlist) = ASTLI_SPTR(listp);
        ASTLI_TRIPLE(newlist) = ASTLI_TRIPLE(listp);
      }
      /* append 2 until the end */
      for (i = 2; i < count; ++i) {
        listp = subs[i];
        newlist = add_astli();
        ASTLI_SPTR(newlist) = ASTLI_SPTR(listp);
        ASTLI_TRIPLE(newlist) = ASTLI_TRIPLE(listp);
      }
    }
    list = ASTLI_HEAD;
    A_LISTP(newforall, list);
    newast = normalize_forall(newforall, srcarray, inlist);
    return newast;

  case I_CSHIFT:  /* cshift(array, shift, [dim]) */
  case I_EOSHIFT: /* eoshift(array, shift, [boundary, dim]); */
    if (A_OPTYPEG(asgn_ast) == I_CSHIFT)
      dim = ARGT_ARG(argt, 2);
    else
      dim = ARGT_ARG(argt, 3);

    srcarray = ARGT_ARG(argt, 0);
    shift = ARGT_ARG(argt, 1);

    if (A_OPTYPEG(asgn_ast) == I_EOSHIFT) {
      boundary = ARGT_ARG(argt, 2);
      if (!boundary)
        ARGT_ARG(argt, 2) = astb.ptr0;
    }

    if (dim == 0)
      dim = mk_cval(1, DT_INT);
    assert(A_TYPEG(shift) == A_CNST,
           "inline_spread_shifts: shift must be constant", 3, shift);
    assert(A_TYPEG(dim) == A_CNST, "inline_spread_shifts: dim must be constant",
           3, dim);
    cdim = get_int_cval(A_SPTRG(dim));
    cshift = get_int_cval(A_SPTRG(shift));
    if (cshift <= 0)
      shift = mk_cval(-1 * cshift, DT_INT);
    retval = normalize_forall(forall_ast, srcarray, inlist);
    asd = A_ASDG(retval);
    ndim = ASD_NDIM(asd);
    list = A_LISTG(forall_ast);
    count = 0;
    for (i = 0; i < ndim; i++) {
      subs[i] = ASD_SUBS(asd, i);
      nidx = 0;
      astli = 0;
      search_forall_idx(ASD_SUBS(asd, i), list, &astli, &nidx);
      if (astli)
        count++;
      if (count == cdim) {
        if (cshift > 0)
          subs[i] = mk_binop(OP_ADD, ASD_SUBS(asd, i), shift, astb.bnd.dtype);
        else
          subs[i] = mk_binop(OP_SUB, ASD_SUBS(asd, i), shift, astb.bnd.dtype);
        count = 99;
      }
    }
    dtype = A_DTYPEG(retval);
    retval = mk_subscr(A_LOPG(retval), subs, ndim, dtype);
    ARGT_ARG(argt, 0) = retval;
    func_ast = asgn_ast;
    retval = mk_func_node(A_TYPEG(func_ast), A_LOPG(func_ast),
                          A_ARGCNTG(func_ast), argt);
    A_DTYPEP(retval, dtype);
    A_SHAPEP(retval, 0);
    A_OPTYPEP(retval, A_OPTYPEG(func_ast));
    return retval;
  case I_SUM: /* sum(a+b,dim=1) */
  case I_PRODUCT:
  case I_MAXVAL:
  case I_MINVAL:
  case I_ALL:
  case I_ANY:
  case I_COUNT:
    srcarray = ARGT_ARG(argt, 0);
    maskarray = ARGT_ARG(argt, 2);
    dim = ARGT_ARG(argt, 1);
    cdim = 0;
    if (dim) {
      cdim = get_int_cval(A_SPTRG(dim));
    }
    assert(cdim, "inline_spread_shifts: reduction intrinsic without dimension",
           3, dim);
    shd = A_SHAPEG(srcarray);
    assert(shd, "inline_spread_shifts: reduction intrinsic without shape", 3,
           shd);
    list = A_LISTG(forall_ast);
    nidx = 1;
    for (listp = list; listp != 0; listp = ASTLI_NEXT(listp))
      ++nidx;
    start_astli();
    listp = list;
    while (nidx) {
      if (nidx == cdim) {
        astli = add_astli();
        ASTLI_SPTR(astli) = 0;
        ASTLI_TRIPLE(astli) =
            mk_triple(SHD_LWB(shd, cdim - 1), SHD_UPB(shd, cdim - 1),
                      SHD_STRIDE(shd, cdim - 1));
      } else {
        astli = add_astli();
        ASTLI_SPTR(astli) = ASTLI_SPTR(listp);
        ASTLI_TRIPLE(astli) = ASTLI_TRIPLE(listp);
        listp = ASTLI_NEXT(listp);
      }
      --nidx;
    }
    newforall = mk_stmt(A_FORALL, 0);
    A_LISTP(newforall, ASTLI_HEAD);
    srcarray = normalize_forall(newforall, srcarray, inlist);
    ARGT_ARG(argt, 0) = srcarray;
    if (maskarray) {
      maskarray = normalize_forall(newforall, maskarray, inlist);
      ARGT_ARG(argt, 2) = maskarray;
    }
    ARGT_ARG(argt, 1) = 0;
    return asgn_ast;
  default:
    dtype = A_DTYPEG(asgn_ast);
    A_DTYPEP(asgn_ast, DDTG(dtype));
    A_SHAPEP(asgn_ast, 0);
    goto ret_norm;
  }
ret_norm:
  for (i = 0; i < nargs; ++i) {
    ARGT_ARG(argt, i) = normalize_forall(forall_ast, ARGT_ARG(argt, i), inlist);
  }
  return asgn_ast;
}

static int
copy_forall(int forall)
{
  int newforall;

  assert(A_TYPEG(forall) == A_FORALL, "copy_forall:must be FORALL", forall, 3);
  newforall = mk_stmt(A_FORALL, 0);
  A_IFSTMTP(newforall, A_IFSTMTG(forall));
  A_IFEXPRP(newforall, A_IFEXPRG(forall));
  A_LISTP(newforall, A_LISTG(forall));
  return newforall;
}

int
make_forall(int shape, int astmem, int mask_ast, int lc)
{
  int i, j, l;
  int numdim;
  int sym;
  int list;
  int triple, triple1;
  int ast, ast1;
  int asd, lwb, upb, stride;
  int dtype;
  int nd;
  int dscast;
  /* Using the array section in shape, create a forall statement that
   * will index it, with the mask_ast as the mask
   */

  numdim = SHD_NDIM(shape);
  if (numdim < 1 || numdim > MAXSUBS) {
    interr("make_forall:bad numdim", shape, 3);
    numdim = 0;
  }
  start_astli();
#ifdef DSCASTG
  switch (A_TYPEG(astmem)) {
  case A_ID:
  case A_LABEL:
  case A_ENTRY:
  case A_SUBSCR:
  case A_SUBSTR:
  case A_MEM:
    dscast = sym_of_ast(astmem);
    dscast = (STYPEG(dscast) == ST_VAR || STYPEG(dscast) == ST_ARRAY)
                 ? DSCASTG(dscast)
                 : 0;
    break;
  default:
    dscast = 0;
  }
#endif

  for (i = numdim - 1; i >= 0; i--) {
/* make each forall index */
#ifdef DSCASTG
    lwb = check_member((dscast) ? dscast : astmem, SHD_LWB(shape, i));
    upb = check_member((dscast) ? dscast : astmem, SHD_UPB(shape, i));
    stride = check_member((dscast) ? dscast : astmem, SHD_STRIDE(shape, i));
#else
    lwb = check_member(astmem, SHD_LWB(shape, i));
    upb = check_member(astmem, SHD_UPB(shape, i));
    stride = check_member(astmem, SHD_STRIDE(shape, i));
#endif
    if (A_DTYPEG(lwb) == DT_INT8 || A_DTYPEG(upb) == DT_INT8 ||
        A_DTYPEG(stride) == DT_INT8)
      dtype = DT_INT8;
    else
      dtype = astb.bnd.dtype;
    /* add the triple */
    /* sym = trans_getidx();*/
    sym = get_init_idx((numdim - 1) - i + lc, dtype);
    list = add_astli();
    triple = mk_triple(lwb, upb, stride);
    ASTLI_SPTR(list) = sym;
    ASTLI_TRIPLE(list) = triple;
  }
  ast = mk_stmt(A_FORALL, 0);
  A_LISTP(ast, ASTLI_HEAD);
  /* now make the mask expression, if any */
  if (mask_ast) {
    ast1 = normalize_forall(ast, mask_ast, 0);
    A_IFEXPRP(ast, ast1);
  } else
    A_IFEXPRP(ast, 0);
  trans_clridx();
  return ast;
}

void
init_tbl(void)
{
  tbl.size = 200;
  NEW(tbl.base, TABLE, tbl.size);
  tbl.avl = 0;
}

void
free_tbl(void)
{
  FREE(tbl.base);
}

int
get_tbl(void)
{
  int nd;

  nd = tbl.avl++;
  NEED(tbl.avl, tbl.base, TABLE, tbl.size, tbl.size + 100);
  if (nd > SPTR_MAX || tbl.base == NULL)
    errfatal(7);
  return nd;
}

#if DEBUG
int *badpointer1 = (int *)0;
long *badpointer2 = (long *)1;
long badnumerator = 99;
long baddenominator = 0;
#endif

void
trans_process_align(void)
{
  int sptr;
  clear_dist_align();
#if DEBUG
  /* convenient place for a segfault */
  if (XBIT(4, 0x2000)) {
    if (!XBIT(4, 0x1000) || gbl.func_count > 2) {
      /* store to null pointer */
      *badpointer1 = 99;
    }
  }
  if (XBIT(4, 0x4000)) {
    if (!XBIT(4, 0x1000) || gbl.func_count > 2) {
      /* divide by zero */
      badnumerator = badnumerator / baddenominator;
    }
  }
  if (XBIT(4, 0x8000)) {
    if (!XBIT(4, 0x1000) || gbl.func_count > 2) {
      /* infinite loop */
      while (badnumerator) {
        badnumerator = (badnumerator < 1) | 3;
      }
    }
  }
#endif
}

static void
trans_get_descrs(void)
{
  int sptr, stype;

  for (sptr = stb.firstusym; sptr < stb.symavl; sptr++) {
    stype = STYPEG(sptr);
    /*	if (stype == ST_ARRAY && SCG(sptr) == SC_NONE)
                NODESCP(sptr, 1);
    */
    /* unused DYNAMIC should be SC_LOCAL */

    if (is_array_type(sptr) && !NODESCG(sptr) && !IGNOREG(sptr)) {
      if (!is_bad_dtype(DTYPEG(sptr)))
        trans_mkdescr(sptr);
    }
  }
}

/* ------------- Utilities ------------ */

/* need to try to reuse indices */
static struct idxlist {
  int idx;
  int free;
  struct idxlist *next;
} * idxlist;

static int
trans_getidx(void)
{
  struct idxlist *p;

  for (p = idxlist; p != 0; p = p->next)
    if (p->free) {
      p->free = 0;
      return p->idx;
    }
  p = (struct idxlist *)getitem(TRANS_AREA, sizeof(struct idxlist));
  p->idx = sym_get_scalar("i", 0, DT_INT);
  p->free = 0;
  p->next = idxlist;
  idxlist = p;
  return p->idx;
}

static void
trans_clridx(void)
{
  struct idxlist *p;

  for (p = idxlist; p != 0; p = p->next)
    p->free = 1;
}

static void
trans_freeidx(void)
{
  idxlist = 0;
  freearea(TRANS_AREA);
}

LOGICAL
is_bad_dtype(int dtype)
{
  if ((DTYG(dtype) != TY_NCHAR) && (DTYG(dtype) != TY_STRUCT) &&
      (DTYG(dtype) != TY_UNION))
    return FALSE;
  return TRUE;
}

LOGICAL
is_array_type(int sptr)
{
  int stype;
  LOGICAL result;

  result = FALSE;
  stype = STYPEG(sptr);
  if ((stype == ST_ARRAY || stype == ST_MEMBER) &&
      DTY(DTYPEG(sptr)) == TY_ARRAY && !DESCARRAYG(sptr))
    result = TRUE;
  return result;
}

static int
find_allocate(int findstd, int findast)
{
  int std, ast;
  for (std = STD_PREV(findstd); std; std = STD_PREV(std)) {
    ast = STD_AST(std);
    if (A_TYPEG(ast) == A_ALLOC && A_TKNG(ast) == TK_ALLOCATE) {
      if (contains_ast(ast, findast)) {
        return std;
      }
    } else if (A_TYPEG(ast) != A_ASN) {
      break;
    }
  }
  return 0;
} /* find_allocate */

static int
find_deallocate(int findstd, int findast)
{
  int std, ast;
  for (std = STD_NEXT(findstd); std; std = STD_NEXT(std)) {
    ast = STD_AST(std);
    if (A_TYPEG(ast) == A_ALLOC && A_TKNG(ast) == TK_DEALLOCATE) {
      if (contains_ast(ast, findast)) {
        return std;
      }
    }
  }
  return 0;
} /* find_deallocate */

/* the function of this routine is to use lhs for user-defined
 * array returning function,
 * allocate (tmp)
 * call user_func(tmp, ..)
 * lhs = tmp + ..
 * deallocate(tmp)
 *    transformed if lhs can be useable
 *  call user_func(lhs, ...)
 *  lhs = lhs + ...
 *
 * lhs is useable
 *   1-lhs is not common
 *   2-lhs is not appear multiply times
 *   3-result is not arg of another function on rhs
 *     (currently, this is checked with contain_calls(rhs)
 *      which is very conservative)
 */
static LOGICAL
use_lhs_for_user_func(int std)
{

  int std1;
  int ast;
  int sptr, lhs_sptr;
  int entry, fval;
  int nargs, argt;
  int ele, a, asd, ndim, i;
  int asn, lhs, src;
  int asn_std, alloc_std, dealloc_std;

  ast = STD_AST(std);
  if (A_TYPEG(ast) != A_CALL)
    return FALSE;
  entry = A_SPTRG(A_LOPG(ast));
  if (!FVALG(entry))
    return FALSE;
  if (PUREG(entry))
    return FALSE;
  if (RECURG(entry))
    return FALSE;
  /* if we are calling an internal function, the internal
   * function might modify the LHS variable directly */
  if (gbl.internal == 1 && INTERNALG(entry))
    return FALSE;
  fval = FVALG(entry);
  if (POINTERG(fval))
    return FALSE;

  nargs = A_ARGCNTG(ast);
  argt = A_ARGSG(ast);
  ele = ARGT_ARG(argt, 0);
  assert(A_TYPEG(ele) == A_ID, "use_lhs_for_user_func: fval not ID", ele, 4);
  sptr = A_SPTRG(ele);

  /* find where ele  is used */
  asn_std = 0;
  for (std1 = STD_NEXT(std); std1; std1 = STD_NEXT(std1)) {
    if (asn_std)
      break;
    ast = STD_AST(std1);
    if (!contains_ast(ast, ele))
      continue;
    if (A_TYPEG(ast) != A_ASN)
      return FALSE;
    asn_std = std1;
  }
  if (!asn_std)
    return FALSE;
  assert(asn_std, "use_lhs_for_user_func: can not find asn", ele, 4);

  alloc_std = dealloc_std = 0;

  if ((!POINTERG(fval) && !ALLOCG(fval)) && (POINTERG(sptr) || ALLOCG(sptr)) &&
      DTY(DTYPEG(sptr)) == TY_ARRAY) {
    /* find where ele is allocated */
    alloc_std = find_allocate(std, ele);
    if (!alloc_std)
      return FALSE;
    assert(alloc_std, "use_lhs_for_user_func: can not find allocate", ele, 4);

    /* find where ele is deallocated */
    dealloc_std = find_deallocate(std, ele);
    assert(dealloc_std, "use_lhs_for_user_func: can not find deallocate", ele,
           4);
  }

  /* decide about whether lhs can be used as function result */
  asn = STD_AST(asn_std);
  lhs = A_DESTG(asn);
  lhs_sptr = sym_of_ast(lhs);
  /* RHS or function might modify array through pointer association */
  if (POINTERG(lhs_sptr))
    return FALSE;
  /* RHS or function might modify array through pointer association */
  if (TARGETG(lhs_sptr))
    return FALSE;
  /* if we are calling an internal function from another internal
   * function and the LHS is from the host subprogram, no */
  if (gbl.internal > 1 && INTERNALG(entry) && !INTERNALG(lhs_sptr))
    return FALSE;
  src = A_SRCG(asn);

  /* need to have same type */
  if (DDTG(DTYPEG(sptr)) != DDTG(DTYPEG(lhs_sptr)))
    return FALSE;

  /* don't allow if lhs appears at rhs */
  if (contains_ast(src, mk_id(lhs_sptr)))
    return FALSE;

  /* don't allow if call has lhs */
  ast = STD_AST(std);
  if (contains_ast(ast, mk_id(lhs_sptr)))
    return FALSE;

  /* don't allow if lhs common */
  if (SCG(lhs_sptr) == SC_CMBLK)
    return FALSE;

  /* don't allow if rhs has call */
  if (contains_call(src))
    return FALSE;

  /* don't allow if the lhs was allocated after the call */
  for (std1 = STD_NEXT(std); std1; std1 = STD_NEXT(std1)) {
    if (std1 == asn_std)
      break;
    ast = STD_AST(std1);
    if (contains_ast(ast, lhs)) {
      return FALSE;
    }
  }

  /* don't allow if any subscript is nontriplet with shape */
  for (a = lhs; a;) {
    switch (A_TYPEG(a)) {
    case A_ID:
      a = 0;
      break;
    case A_MEM:
      a = A_PARENTG(a);
      break;
    case A_SUBSTR:
    default:
      return FALSE;

    case A_SUBSCR:
      asd = A_ASDG(a);
      ndim = ASD_NDIM(asd);
      for (i = 0; i < ndim; ++i) {
        int ss = ASD_SUBS(asd, i);
        if (A_SHAPEG(ss) != 0 && A_TYPEG(ss) != A_TRIPLE) {
          /* vector subscript, ugly */
          return FALSE;
        }
      }
      a = A_LOPG(a);
      break;
    }
  }

  ast_visit(1, 1);
  ast_replace(ele, lhs);
  if (A_SRCG(asn) == ele) {
    /* don't change tmp(:) = F(b(:)) ; a(:) = tmp(:)
     * into a(:) = F(b(:)) ; a(:) = a(:) */
    delete_stmt(asn_std);
  } else {
    /* change the asn */
    asn = ast_rewrite(asn);
    STD_AST(asn_std) = asn;
  }

  /* change the call */
  ast = STD_AST(std);
  ast = ast_rewrite(ast);
  STD_AST(std) = ast;

  ast_unvisit();

  /* delete allocate and deallocate */
  if (alloc_std)
    delete_stmt(alloc_std);
  if (dealloc_std)
    delete_stmt(dealloc_std);
  return TRUE;
}

/* if the array bounds, or distribute arguments of this template
 * contain any variables, return TRUE */
static LOGICAL
variable_template(int tmpl)
{
  int dtype, dist, i, b;
  dtype = DTYPEG(tmpl);
  if (DTY(dtype) == TY_ARRAY) {
    for (i = 0; i < ADD_NUMDIM(dtype); ++i) {
      b = ADD_LWAST(dtype, i);
      if (b && A_ALIASG(b) == 0)
        return TRUE;
      b = ADD_UPAST(dtype, i);
      if (!b || A_ALIASG(b) == 0)
        return TRUE;
    }
  }
  return FALSE;
} /* variable_template */

/* replace dummy arguments in an alignment descriptor with actual arguments */
static int find_entry, find_nargs, find_argt, find_dpdsc, find_std;

static void
find_args(int ast, int *extra)
{
  if (A_TYPEG(ast) == A_ID && A_REPLG(ast) == 0) {
    /* is this a dummy argument? */
    int sptr, i;
    sptr = A_SPTRG(ast);
    for (i = 0; i < find_nargs; ++i) {
      int arg;
      arg = aux.dpdsc_base[find_dpdsc + i];
      if (sptr == arg) {
        /* we need to make a copy; get a temp */
        int temp, dtype, assn, actual;
        char *tempname;
        dtype = DTYPEG(sptr);
        actual = ARGT_ARG(find_argt, i);
        if (DTY(dtype) != TY_ARRAY) {
          if (actual && A_DTYPEG(actual) == dtype) {
            if (A_ALIASG(actual) && dtype == DT_INT) {
              ast_replace(ast, A_ALIASG(actual));
            } else {
              tempname = mangle_name(SYMNAME(sptr), "t");
              temp = getsymbol(tempname);
              STYPEP(temp, ST_VAR);
              DCLDP(temp, 1);
              SCP(temp, SC_LOCAL);
              DTYPEP(temp, dtype);
              /* copy from i'th actual argument */
              assn = mk_assn_stmt(mk_id(temp), ARGT_ARG(find_argt, i), dtype);
              add_stmt_before(assn, find_std);
              ast_replace(ast, mk_id(temp));
            }
          }
        } else {
          /* only handle if the actual is itself an array */
          if (A_TYPEG(actual) == A_ID) {
            /* must be same type of array */
            int adtype;
            adtype = A_DTYPEG(actual);
            if (DTY(adtype + 1) == DTY(dtype + 1)) {
              /* use the actual argument */
              ast_replace(ast, actual);
            }
          }
        }
      }
    }
  }
} /* find_args */

static void
find_arguments(int std, int entry, int nargs, int argt, int ast)
{
  if (PARAMCTG(entry) != nargs || ast == 0)
    return;
  find_entry = entry;
  find_dpdsc = DPDSCG(entry);
  if (find_dpdsc == 0)
    return;
  find_nargs = nargs;
  find_argt = argt;
  find_std = std;
  ast_traverse(ast, NULL, find_args, NULL);
} /* replace_arguments */

static LOGICAL
is_non0_scope(int sptr)
{
  int stype;
  int dtype;
  ADSC *ad;
  int ndim, i;
  int lb, ub, ast;
  int proc, tmpl;
  int dist, align;

  stype = STYPEG(sptr);
  if (IGNOREG(sptr))
    return TRUE;
  if (stype == ST_ARRAY) {
    dtype = DTYPEG(sptr);
    ad = AD_DPTR(dtype);
    ndim = AD_NUMDIM(ad);
    for (i = 0; i < ndim; ++i) {
      lb = AD_LWBD(ad, i);
      if (contains_non0_scope(lb))
        return TRUE;
      lb = AD_LWAST(ad, i);
      if (contains_non0_scope(lb))
        return TRUE;
      ub = AD_UPBD(ad, i);
      if (contains_non0_scope(ub))
        return TRUE;
      ub = AD_UPAST(ad, i);
      if (contains_non0_scope(ub))
        return TRUE;
    }
  }
  return FALSE;
}

/* This is the callback function for contains_non0_scope(). */
static LOGICAL
_contains_non0_scope(int astSrc, LOGICAL *pflag)
{
  if (astSrc && A_TYPEG(astSrc) == A_ID && IGNOREG(A_SPTRG(astSrc))) {
    *pflag = TRUE;
    return TRUE;
  }
  return FALSE;
}

/* Return TRUE if astSrc has non zero scope ID somewhere within astSrc.
 */
static LOGICAL
contains_non0_scope(int astSrc)
{
  LOGICAL result = FALSE;

  if (!astSrc)
    return FALSE;

  ast_visit(1, 1);
  ast_traverse(astSrc, _contains_non0_scope, NULL, &result);
  ast_unvisit();
  return result;
}

static void
_copy(int ast, int *unused)
{
  if (DT_ISINT(A_DTYPEG(ast))) {
    int sptr;
    /* member reference, subscript, simple ID? */
    switch (A_TYPEG(ast)) {
    case A_ID:
    case A_SUBSCR:
    case A_MEM:
      /* not section descriptor, not compiler temp */
      sptr = memsym_of_ast(ast);
      if (!DESCARRAYG(sptr) && !CCSYMG(sptr) && !HCCSYMG(sptr)) {
        /* not already copied */
        if (A_REPLG(ast) == 0) {
          int tmp, newast, ent;
          tmp = getcctmp('d', ast, ST_VAR, DT_INT);
          newast = mk_id(tmp);
          for (ent = gbl.entries; ent != NOSYM; ent = SYMLKG(ent)) {
            int entry, asn;
            entry = ENTSTDG(ent);
            asn = mk_assn_stmt(newast, ast, DT_INT);
            add_stmt_after(asn, entry);
          }
          ast_replace(ast, newast);
        }
      }
      break;
    }
  }
} /* _copy */

static int
copy_nonconst(int ast)
{
  int newast;
  if (ast == 0)
    return 0;
  if (A_TYPEG(ast) == A_CNST)
    return ast;

  /* anything else, search, replace */
  ast_traverse(ast, NULL, _copy, NULL);
  newast = ast_rewrite(ast);
  return newast;
} /* copy_nonconst */

/* Make an AST id for the descriptor (SDSC or DESCR) of this symbol. */
static int
mk_descr_id(SPTR sptr)
{
  if (SDSCG(sptr)) {
    return mk_id(SDSCG(sptr));
  } else if (DESCRG(sptr)) {
    return mk_id(DESCRG(sptr));
  } else {
    interr("no descriptor for symbol", sptr, ERR_Fatal);
    return 0;
  }
}

static int
build_sdsc_node(int ast)
{
  SPTR sptr = sym_of_ast(ast);
  int astsdsc;
  if (A_TYPEG(ast) == A_SUBSCR)
    ast = A_LOPG(ast);
  if (A_TYPEG(ast) == A_MEM) {
    SPTR sptrmem = memsym_of_ast(ast);
    int astparent = A_PARENTG(ast);
    astsdsc = mk_id(SDSCG(sptrmem));
    astsdsc = mk_member(astparent, astsdsc, DTYPEG(sptr));
  } else {
    astsdsc = mk_descr_id(sptr);
  }
  return astsdsc;
}

static int
build_conformable_func_node(int astdest, int astsrc)
{
  int ast;
  int astfunc;
  int astdestsdsc;
  int astsrcsdsc;
  int sptrdestmem = memsym_of_ast(astdest);
  int sptrsrcmem = 0;
  int sptr;
  int sptrfunc;
  int argt;
  int dtypesrc = A_DTYPEG(astsrc);
  int dtypedest = A_DTYPEG(astdest);
  int srcshape = A_SHAPEG(astsrc);
  int i;
  int nargs;

  if (A_TYPEG(astsrc) == A_ID || A_TYPEG(astsrc) == A_CONV ||
      A_TYPEG(astsrc) == A_CNST || A_TYPEG(astsrc) == A_MEM) {
    sptrsrcmem = memsym_of_ast(astsrc);
  }

  astdestsdsc = 0;
  if (DESCUSEDG(sptrdestmem)) {
    astdestsdsc = build_sdsc_node(astdest);
  } else if (SCG(sptrdestmem) == SC_DUMMY && NEWDSCG(sptrdestmem)) {
    astdestsdsc = mk_id(NEWDSCG(sptrdestmem));
  }

  astsrcsdsc = 0;
  if (sptrsrcmem) {
    if (DESCUSEDG(sptrsrcmem)) {
      astsrcsdsc = build_sdsc_node(astsrc);
    } else if (SCG(sptrsrcmem) == SC_DUMMY && NEWDSCG(sptrsrcmem)) {
      astsrcsdsc = mk_id(NEWDSCG(sptrsrcmem));
    }
  }

  if (astdestsdsc) {
    if (astsrcsdsc) {
      nargs = 3;
      argt = mk_argt(nargs);
      ARGT_ARG(argt, 0) = astdest;
      ARGT_ARG(argt, 1) = astdestsdsc;
      ARGT_ARG(argt, 2) = astsrcsdsc;
      sptrfunc = sym_mkfunc(mkRteRtnNm(RTE_conformable_dd), DT_INT);
    } else {
      int ndim;
      if (A_SHAPEG(astsrc)) {
        ndim = SHD_NDIM(srcshape);
        nargs = 3 + ndim;
        argt = mk_argt(nargs);
        ARGT_ARG(argt, 0) = astdest;
        ARGT_ARG(argt, 1) = astdestsdsc;
        ARGT_ARG(argt, 2) = mk_cval(ndim, astb.bnd.dtype);
        for (i = 0; i < ndim; i++) {
          ARGT_ARG(argt, 3 + i) =
              mk_extent_expr(SHD_LWB(srcshape, i), SHD_UPB(srcshape, i));
        }
        sptrfunc = sym_mkfunc(mkRteRtnNm(RTE_conformable_dn), DT_INT);
      } else {
        /* array = scalar
         * generate
         *    RTE_conformable_dd(dest_addr, dest_sdsc, dest_sdsc)
         * will return false iff array is not allocated (i.e., the conformable
         * call is an RTE_allocated call) */
        nargs = 3;
        argt = mk_argt(nargs);
        ARGT_ARG(argt, 0) = astdest;
        ARGT_ARG(argt, 1) = astdestsdsc;
        ARGT_ARG(argt, 2) = astdestsdsc;
        sptrfunc = sym_mkfunc(mkRteRtnNm(RTE_conformable_dd), DT_INT);
      }
    }
  } else {
    if (astsrcsdsc) {
      int ndim = ADD_NUMDIM(dtypesrc);
      nargs = 3 + ndim;
      argt = mk_argt(nargs);
      ARGT_ARG(argt, 0) = astdest;
      ARGT_ARG(argt, 1) = astsrcsdsc;
      ARGT_ARG(argt, 2) = mk_cval(ndim, astb.bnd.dtype);
      for (i = 0; i < ndim; i++) {
        ARGT_ARG(argt, 3 + i) =
            mk_extent_expr(ADD_LWAST(dtypedest, i), ADD_UPAST(dtypedest, i));
      }
      sptrfunc = sym_mkfunc(mkRteRtnNm(RTE_conformable_nd), DT_INT);
    } else {
      int ndim;
      if (A_SHAPEG(astsrc)) {
        /* generate
         *  RTE_conformable_nn(dest_addr, dest_sz, dest_sz, dest_ndim,
         *                       dest_extnt1,src_extnt1, ...,
         * dest_extntn,src_extntn) */
        ndim = SHD_NDIM(srcshape);
        nargs = 2 + 2 * ndim;
        argt = mk_argt(nargs);
        ARGT_ARG(argt, 0) = astdest;
        ARGT_ARG(argt, 1) = mk_cval(ndim, astb.bnd.dtype);
        for (i = 0; i < ndim; i++) {
          ARGT_ARG(argt, 2 + i * 2) =
              mk_extent_expr(ADD_LWAST(dtypedest, i), ADD_UPAST(dtypedest, i));
          ARGT_ARG(argt, 3 + i * 2) =
              mk_extent_expr(SHD_LWB(srcshape, i), SHD_UPB(srcshape, i));
        }
      } else {
        /* array = scalar
         * generate
         *  RTE_conformable_nn(dest_addr, dest_sz, dest_sz, dest_ndim,
         *  dest_extnt1,dest_extnt1, ..., dest_extntn,dest_extntn)
         * will return false iff array is not allocated (i.e., the conformable
         * call acts as a RTE_allocated call) */
        ndim = ADD_NUMDIM(dtypedest);
        nargs = 2 + 2 * ndim;
        argt = mk_argt(nargs);
        ARGT_ARG(argt, 0) = astdest;
        ARGT_ARG(argt, 1) = mk_cval(ndim, astb.bnd.dtype);
        for (i = 0; i < ndim; i++) {
          ARGT_ARG(argt, 2 + i * 2) =
              mk_extent_expr(ADD_LWAST(dtypedest, i), ADD_UPAST(dtypedest, i));
          ARGT_ARG(argt, 3 + i * 2) = ARGT_ARG(argt, 2 + i * 2);
        }
      }
      sptrfunc = sym_mkfunc(mkRteRtnNm(RTE_conformable_nn), DT_INT);
    }
  }

  NODESCP(sptrfunc, 1);
  astfunc = mk_id(sptrfunc);
  A_DTYPEP(astfunc, DT_INT);
  ast = mk_func_node(A_FUNC, astfunc, nargs, argt);
  A_DTYPEP(ast, DT_INT);
  A_OPTYPEP(ast, INTASTG(sptrfunc));
  A_LOPP(ast, astfunc);

  return ast;
}

void
rewrite_deallocate(int ast, int std)
{
  int i;
  int sptrmem;
  DTYPE dtype = A_DTYPEG(ast);
  int shape = A_SHAPEG(ast);
  int astparent = ast;
  int docnt = 0;
  LOGICAL need_endif = FALSE;

  assert(DTY(DDTG(dtype)) == TY_DERIVED, "unexpected dtype", dtype, ERR_Fatal);
  if (ALLOCATTRG(memsym_of_ast(ast))) {
    gen_allocated_check(ast, std, A_IFTHEN, FALSE);
    need_endif = TRUE;
  }
  if (shape != 0) {
    int asd;
    assert(DTY(dtype) == TY_ARRAY, "expecting array dtype", 0, ERR_Fatal);
    assert(all_stride_one_shape(shape), "shape not all stride 1", 0, ERR_Fatal);
    asd = gen_dos_over_shape(shape, std);
    docnt = ASD_NDIM(asd);
    if (A_TYPEG(ast) == A_MEM) {
      astparent = subscript_allocmem(ast, asd);
    } else {
      astparent = mk_subscr_copy(ast, asd, DTY(dtype + 1));
    }
  }

  for (sptrmem = DTY(DDTG(dtype) + 1); sptrmem > NOSYM;
       sptrmem = SYMLKG(sptrmem)) {
    int astdealloc;
    int astmem;
    if (CLASSG(sptrmem) && VTABLEG(sptrmem) &&
        (BINDG(sptrmem) || FINALG(sptrmem))) {
      continue; /* skip tbp */
    }
    astmem = mk_id(sptrmem);
    astmem = mk_member(astparent, astmem, A_DTYPEG(astmem));
    if (!POINTERG(sptrmem) && allocatable_member(sptrmem)) {
      rewrite_deallocate(astmem, std);
    }
    if (!ALLOCATTRG(sptrmem)) {
      continue;
    }
    astdealloc = mk_stmt(A_ALLOC, 0);
    A_TKNP(astdealloc, TK_DEALLOCATE);
    A_DALLOCMEMP(astdealloc, 1);
    A_SRCP(astdealloc, astmem);
    add_stmt_before(astdealloc, std);
  }

  gen_do_ends(docnt, std);
  if (need_endif) {
    int astendif = mk_stmt(A_ENDIF, 0);
    add_stmt_before(astendif, std);
  }
}

/** \brief Generate an IF to see if ast is allocated and insert before std.
           Caller is responsible for generating ENDIF.
    \param atype  Type of AST to generate, A_IFTHEN or A_ELSEIF.
    \param negate Check for not allocated instead of allocated.
 */
static void
gen_allocated_check(int ast, int std, int atype, LOGICAL negate)
{
  int astfunc;
  int funcid = mk_id(getsymbol("allocated"));
  int argt = mk_argt(1);
  int astif = mk_stmt(atype, 0);

  assert(atype == A_IFTHEN || atype == A_ELSEIF, "Bad ast type", atype, ERR_Fatal);
  A_DTYPEP(funcid, DT_LOG);
  ARGT_ARG(argt, 0) = A_TYPEG(ast) == A_SUBSCR ? A_LOPG(ast) : ast;
  astfunc = mk_func_node(A_INTR, funcid, 1, argt);
  A_DTYPEP(astfunc, DT_LOG);
  A_OPTYPEP(astfunc, I_ALLOCATED);
  if (negate)
    astfunc = mk_unop(OP_LNOT, astfunc, DT_LOG);
  A_IFEXPRP(astif, astfunc);
  add_stmt_before(astif, std);
}

/* Generate DOs over each dimension of shape, insert then before std,
   and return the temp loop variables as an ASD. */
static int
gen_dos_over_shape(int shape, int std)
{
  int i;
  int subs[MAXSUBS];
  int ndim = SHD_NDIM(shape);
  for (i = 0; i < ndim; i++) {
    int astdo = mk_stmt(A_DO, 0);
    int sub = mk_id(get_temp(astb.bnd.dtype));
    A_DOVARP(astdo, sub);
    A_M1P(astdo, SHD_LWB(shape, i));
    A_M2P(astdo, SHD_UPB(shape, i));
    A_M3P(astdo, SHD_STRIDE(shape, i));
    A_M4P(astdo, 0);
    add_stmt_before(astdo, std);
    subs[i] = sub;
  }
  return mk_asd(subs, ndim);
}

static void
gen_do_ends(int docnt, int std)
{
  int astdo;
  int i;

  for (i = 0; i < docnt; i++) {
    astdo = mk_stmt(A_ENDDO, 0);
    add_stmt_before(astdo, std);
  }
}

static void
gen_bounds_assignments(int astdestparent, int astdestmem, int astsrcparent,
                       int astsrcmem, int std)
{
  int sptrdest;
  int ndim = 0;
  int shape;

  if (is_array_dtype(A_DTYPEG(astdestmem)))
    ndim = ADD_NUMDIM(A_DTYPEG(astdestmem));

  if (!astdestparent && A_TYPEG(astdestmem) == A_MEM) {
    astdestparent = A_PARENTG(astdestmem);
    astdestmem = A_MEMG(astdestmem);
  }

  if (astsrcparent && SDSCG(A_SPTRG(astsrcmem))) {
    shape = mk_mem_ptr_shape(astsrcparent, astsrcmem, A_DTYPEG(astsrcmem));
  } else {
    shape = A_SHAPEG(astsrcmem);
  }
  if (shape == 0 && astsrcparent != 0) {
    shape = A_SHAPEG(astsrcparent);
  }
  if (shape == 0) {
    assert(ndim == 0, "unexpected ndim", ndim, ERR_Fatal);
    return;
  }
  assert(ndim == SHD_NDIM(shape), "bad shape", 0, ERR_Fatal);
  if (!all_stride_one_shape(shape)) {
    shape = mk_bounds_shape(shape);
  }

  sptrdest = memsym_of_ast(astdestmem);
  if (DESCUSEDG(sptrdest)) {
    int i;
    int astdest = mk_descr_id(sptrdest);
    if (astdestparent) {
      astdest = mk_member(astdestparent, astdest, astb.bnd.dtype);
    }
    for (i = 0; i < ndim; i++) {
      int stride = SHD_STRIDE(shape, i);
      int astlb = SHD_LWB(shape, i);
      int astub = SHD_UPB(shape, i);
      int astextnt = extent_of_shape(shape, i);
      int subscr = mk_cval(get_global_lower_index(i), astb.bnd.dtype);
      int ast = mk_subscr(astdest, &subscr, 1, astb.bnd.dtype);
      ast = mk_assn_stmt(ast, astlb, astb.bnd.dtype);
      add_stmt_before(ast, std);
      subscr = mk_cval(get_global_upper_index(i), astb.bnd.dtype);
      ast = mk_subscr(astdest, &subscr, 1, astb.bnd.dtype);
      ast = mk_assn_stmt(ast, astub, astb.bnd.dtype);
      add_stmt_before(ast, std);
      subscr = mk_cval(get_global_extent_index(i), astb.bnd.dtype);
      ast = mk_subscr(astdest, &subscr, 1, astb.bnd.dtype);
      ast = mk_assn_stmt(ast, astextnt, astb.bnd.dtype);
      add_stmt_before(ast, std);
    }
    if (DDTG(A_DTYPEG(A_DESTG(STD_AST(std)))) == DT_DEFERCHAR) {
      int lhs_len = get_len_of_deferchar_ast(A_DESTG(STD_AST(std)));
      int rhs_len = string_expr_length(A_SRCG(STD_AST(std)));
      int ast = mk_assn_stmt(lhs_len, rhs_len, DT_INT);
      add_stmt_before(ast, std);
    }
  } else {
    int i;
    DTYPE dtypedest = DTYPEG(sptrdest);
    for (i = 0; i < ndim; i++) {
      int astlb = SHD_LWB(shape, i);
      int astub = SHD_UPB(shape, i);
      int astextnt = extent_of_shape(shape, i);
      int ast = mk_assn_stmt(ADD_LWBD(dtypedest, i), astlb, astb.bnd.dtype);
      add_stmt_before(ast, std);
      ast = mk_assn_stmt(ADD_UPBD(dtypedest, i), astub, astb.bnd.dtype);
      add_stmt_before(ast, std);
      ast = mk_assn_stmt(ADD_EXTNTAST(dtypedest, i), astextnt, astb.bnd.dtype);
      add_stmt_before(ast, std);
    }
  }
}

/* Is the stride 1 in every dimension of shape? */
static LOGICAL
all_stride_one_shape(int shape)
{
  int i;
  int ndim = SHD_NDIM(shape);
  for (i = 0; i < ndim; i++) {
    int stride = SHD_STRIDE(shape, i);
    if (stride != astb.bnd.one)
      return FALSE;
  }
  return TRUE;
}

/* Make a new shape using 1:extent for the dimensions without stride 1 */
static int
mk_bounds_shape(int shape)
{
  int i;
  int ndim = SHD_NDIM(shape);
  add_shape_rank(ndim);
  for (i = 0; i < ndim; i++) {
    int stride = SHD_STRIDE(shape, i);
    int lb;
    int ub;
    if (stride == astb.bnd.one) {
      lb = SHD_LWB(shape, i);
      ub = SHD_UPB(shape, i);
    } else {
      lb = astb.bnd.one;
      ub = extent_of_shape(shape, i);
    }
    add_shape_spec(lb, ub, astb.bnd.one);
  }
  return mk_shape();
}

static int
build_allocation_item(int astdestparent, int astdestmem)
{
  int indx[MAXSUBS];
  int ndim;
  int astitem;
  int sptrdest;
  int sptrsdsc;
  int astdest;
  int astsdsc;
  int i;
  int subscr;
  int lbast;
  int ubast;

  sptrdest = memsym_of_ast(astdestmem);
  if (DTY(DTYPEG(sptrdest)) != TY_ARRAY) {
    if (STYPEG(sptrdest) == ST_MEMBER && astdestparent) {
      /* FS#20128: astdestmem is an allocatable scalar */
      return mk_member(astdestparent, astdestmem, A_DTYPEG(astdestmem));
    }
    return astdestmem;
  }

  if (A_TYPEG(astdestmem) == A_SUBSCR)
    astdestmem = A_LOPG(astdestmem);
  ndim = ADD_NUMDIM(A_DTYPEG(astdestmem));

  astdest = astdestmem;
  if (astdestparent) {
    astdest = mk_member(astdestparent, astdest, astb.bnd.dtype);
  } else if (!astdestparent && A_TYPEG(astdestmem) == A_MEM) {
    astdestparent = A_PARENTG(astdestmem);
    astdestmem = A_MEMG(astdestmem);
  }

  if (DESCUSEDG(sptrdest)) {
    astsdsc = mk_descr_id(memsym_of_ast(astdestmem));
    if (astdestparent) {
      astsdsc = mk_member(astdestparent, astsdsc, astb.bnd.dtype);
    }
    for (i = 0; i < ndim; i++) {
      subscr = mk_cval(get_global_lower_index(i), astb.bnd.dtype);
      lbast = mk_subscr(astsdsc, &subscr, 1, astb.bnd.dtype);
      subscr = mk_cval(get_global_upper_index(i), astb.bnd.dtype);
      ubast = mk_subscr(astsdsc, &subscr, 1, astb.bnd.dtype);
      indx[i] = mk_triple(lbast, ubast, astb.i1);
    }
  } else {
    int dtypedest = DTYPEG(sptrdest);
    for (i = 0; i < ndim; i++) {
      indx[i] =
          mk_triple(ADD_LWBD(dtypedest, i), ADD_UPBD(dtypedest, i), astb.i1);
    }
  }
  astitem = mk_subscr(astdest, indx, ndim, DTYG(A_DTYPEG(astdestmem)));

  return astitem;
}

static void
gen_alloc_mbr(int ast, int std)
{
  int astfunc;
  int sptr;

  astfunc = mk_stmt(A_ALLOC, 0);
  A_TKNP(astfunc, TK_ALLOCATE);
  A_SRCP(astfunc, ast);
  add_stmt_before(astfunc, std);

  sptr = memsym_of_ast(ast);
  check_alloc_ptr_type(sptr, std, DTYPEG(sptr), 1, 0, 0, ast);
}

static void
gen_dealloc_mbr(int ast, int std)
{
  int astfunc;
  int std_dealloc;

  astfunc = mk_stmt(A_ALLOC, 0);
  A_TKNP(astfunc, TK_DEALLOCATE);
  A_SRCP(astfunc, ast);
  A_DALLOCMEMP(astfunc, 1);
  std_dealloc = add_stmt_before(astfunc, std);
  if (allocatable_member(memsym_of_ast(ast))) {
    rewrite_deallocate(ast, std_dealloc);
  }
}

static void
nullify_member(int ast, int std, int sptr)
{
  int dtype = DTYPEG(sptr);
  int sptrmem, aast, mem_sptr_id;

  for (sptrmem = DTY(DDTG(dtype) + 1); sptrmem > NOSYM;
       sptrmem = SYMLKG(sptrmem)) {
    if (ALLOCATTRG(sptrmem)) {
      aast = mk_id(sptrmem);
      mem_sptr_id = mk_member(ast, aast, DTYPEG(sptrmem));
      add_stmt_before(add_nullify_ast(mem_sptr_id), std);
    }
    if (CLASSG(sptrmem) && VTABLEG(sptrmem) &&
        (BINDG(sptrmem) || FINALG(sptrmem))) {
      /* skip tbp */
      continue;
    }
  }
}

static void
handle_allocatable_members(int astdest, int astsrc, int std,
                           LOGICAL non_conformable)
{
  int sptrmem;
  int docnt = 0;
  int astdestparent = astdest;
  int astsrcparent = astsrc;
  DTYPE dtype = A_DTYPEG(astdest);
  int shape = A_SHAPEG(astdest);

  if (shape != 0) {
    int destasd;
    int srcasd;
    if (A_TYPEG(astdest) == A_MEM) {
      int memsptr = A_SPTRG(A_MEMG(astdest));
      if (POINTERG(memsptr) || ALLOCATTRG(memsptr)) {
        shape = mk_mem_ptr_shape(A_PARENTG(astdest), A_MEMG(astdest), dtype);
      }
    }
    destasd = gen_dos_over_shape(shape, std);
    docnt = ASD_NDIM(destasd);
    srcasd = normalize_subscripts(destasd, shape, A_SHAPEG(astsrc));
    astdestparent = subscript_allocmem(astdest, destasd);
    if (A_SHAPEG(astsrc)) {
      astsrcparent = subscript_allocmem(astsrc, srcasd);
    }
  }

  for (sptrmem = DTY(DDTG(dtype) + 1); sptrmem > NOSYM;
       sptrmem = SYMLKG(sptrmem)) {
    /* for allocatable components, build an assignment and recurse */
    int astmem;
    int astdestcmpnt;
    int astsrccmpnt;
    if (CLASSG(sptrmem) && (BINDG(sptrmem) || FINALG(sptrmem)) &&
        VTABLEG(sptrmem)) {
      continue; /* skip tbp */
    }
    astmem = mk_id(sptrmem);
    astdestcmpnt = mk_member(astdestparent, astmem, A_DTYPEG(astmem));
    astsrccmpnt = mk_member(astsrcparent, astmem, A_DTYPEG(astmem));

    if (A_SHAPEG(astmem) && DESCUSEDG(sptrmem) &&
        !(USELENG(sptrmem) && ALLOCG(sptrmem) && TPALLOCG(sptrmem))) {
      int destshape = mk_mem_ptr_shape(astdestparent, astmem, A_DTYPEG(astmem));
      int srcshape = mk_mem_ptr_shape(astsrcparent, astmem, A_DTYPEG(astmem));
      A_SHAPEP(astdestcmpnt, destshape);
      A_SHAPEP(astsrccmpnt, srcshape);
    }
    if (POINTERG(sptrmem) && !F90POINTERG(sptrmem)) {
      int ptr_assign = add_ptr_assign(astdestcmpnt, astsrccmpnt, std);
      A_SHAPEP(ptr_assign, A_SHAPEG(astsrccmpnt));
      add_stmt_before(ptr_assign, std);
    } else {
      int stdassncmpnt;
      int sym = memsym_of_ast(astdest);
      int mem = memsym_of_ast(astdestcmpnt);
      int assn = mk_assn_stmt(astdestcmpnt, astsrccmpnt, A_DTYPEG(astsrccmpnt));
      A_SHAPEP(assn, A_SHAPEG(astsrccmpnt));
      stdassncmpnt = add_stmt_before(assn, std);

      if (SCG(sym) == SC_LOCAL && !INMODULEG(sym) && !SAVEG(sym) &&
          A_TYPEG(astdest) == A_SUBSCR &&
          (ALLOCATTRG(mem) || allocatable_member(mem))) {
        /* FS#19743: Make sure this member is NULL. Since we're
         * accessing a member in an individual element of an array
         * of derived type, we need to make sure member is initially
         * NULL here.
         */
        int i;
        LOGICAL const_subscript = FALSE;
        int asd = A_ASDG(astdest);
        int ndim = ASD_NDIM(asd);
        for (i = 0; i < ndim; i++) {
          const_subscript = A_TYPEG(ASD_SUBS(asd, i)) == A_CNST;
          if (!const_subscript)
            break;
        }
        if (const_subscript) {
          add_stmt_after(add_nullify_ast(astdestcmpnt), ENTSTDG(gbl.currsub));
        }
      }

      if ((ALLOCATTRG(sptrmem) || allocatable_member(sptrmem)) &&
          !TPALLOCG(sptrmem)) {
        rewrite_allocatable_assignment(assn, stdassncmpnt, non_conformable);
      }
    }

    if (ALLOCG(sptrmem) || (POINTERG(sptrmem) && !F90POINTERG(sptrmem))) {
      /* skip past $p, $o, $sd $td */
      int osptr = sptrmem;
      int midnum = MIDNUMG(sptrmem);
      int offset = PTROFFG(sptrmem);
      int sdsc = SDSCG(sptrmem);
      if (sdsc && STYPEG(sdsc) == ST_MEMBER) {
        if (SYMLKG(sptrmem) == midnum) {
          sptrmem = SYMLKG(sptrmem);
        }
        if (SYMLKG(sptrmem) == offset) {
          sptrmem = SYMLKG(sptrmem);
        }
        if (SYMLKG(sptrmem) == sdsc) {
          sptrmem = SYMLKG(sptrmem);
        }
        if (CLASSG(osptr) && DESCARRAYG(sptrmem)) {
          sptrmem = SYMLKG(sptrmem);
        }
      } else {
        if (midnum && midnum == SYMLKG(sptrmem))
          sptrmem = SYMLKG(sptrmem);
        if (sdsc && sdsc == SYMLKG(sptrmem))
          sptrmem = SYMLKG(sptrmem);
      }
    }
  }

  gen_do_ends(docnt, std);
}

static int sptrMatch;   /* sptr # for matching */
static int parentMatch; /* sptr # for matching */

/* This is the callback function for contains_sptr(). */
static LOGICAL
_contains_sptr(int astSrc, LOGICAL *pflag)
{
  if (A_TYPEG(astSrc) == A_ID && sptrMatch == A_SPTRG(astSrc) &&
      parentMatch == 0) {
    *pflag = TRUE;
    return TRUE;
  } else if (A_TYPEG(astSrc) == A_MEM && sptrMatch == A_SPTRG(astSrc) &&
             parentMatch == A_PARENTG(astSrc)) {
    *pflag = TRUE;
    return TRUE;
  }
  return FALSE;
}

/* Return TRUE if sptrDst occurs somewhere within astSrc. */
static LOGICAL
contains_sptr(int astSrc, int sptrDst, int astparent)
{
  LOGICAL result = FALSE;

  if (!astSrc)
    return FALSE;

  sptrMatch = sptrDst;
  parentMatch = astparent;
  ast_visit(1, 1);
  ast_traverse(astSrc, _contains_sptr, NULL, &result);
  ast_unvisit();
  return result;
}

/* MORE - possible performance improvements:
 *   1) The RTE_conformable_* RTL functions' return values are ternary
 * returning
 *        1 ==> conformable
 *        0 ==> not conformable but big enough
 *       -1 --> not conformable, no big enough
 *       but the code generated below collapses values 0 and -1 into "not
 * conformable".
 *       An "ALLOCATE" could be saved by separating these two states (would need
 * to
 *       reset bounds variables and "remember" actual allocation size).
 *   2) check assignments to allocatable arrays where the shape of the  RHS is
 *      known to be compatiable with the LHS,  e.g.,
 *        alloc_array = alloc_array + scalar_value
 *      in this case nothing needs to be done
 *   3) optimize assignments of derived type initializers, e.g.,
 *      derived_type%alloc_component = (prototype instance)%alloc_component
 */
static void
rewrite_allocatable_assignment(int astasgn, const int std, LOGICAL non_conformable)
{
  int sptrdest;
  int shape;
  int indx[MAXSUBS];
  int astdestparent;
  int astsrcparent;
  int astif;
  int ast;
  int asttmp;
  int sptrsrc = NOSYM;
  DTYPE dtype = A_DTYPEG(astasgn);
  int astdest = A_DESTG(astasgn);
  DTYPE dtypedest = A_DTYPEG(astdest);
  int astsrc = A_SRCG(astasgn);
  DTYPE dtypesrc = A_DTYPEG(astsrc);
  int stdnext = STD_NEXT(std);
  LOGICAL alloc_scalar_parent_only = FALSE;

again:
  if (A_TYPEG(astdest) != A_ID && A_TYPEG(astdest) != A_MEM &&
      A_TYPEG(astdest) != A_CONV && A_TYPEG(astdest) != A_SUBSCR) {
    return;
  }
  if (A_TYPEG(astdest) == A_SUBSCR && DTYG(A_DTYPEG(astdest)) != TY_DERIVED) {
    return;
  }
  if (A_TYPEG(astsrc) == A_FUNC) {
    if (!XBIT(54, 0x1)) {
      if (A_DTYPEG(astdest) == DT_DEFERCHAR ||
          A_DTYPEG(astdest) == DT_DEFERNCHAR) {
        int fval = FVALG(A_SPTRG(A_LOPG(astsrc)));
        if (DTYPEG(fval) == DT_DEFERCHAR || DTYPEG(fval) == DT_DEFERNCHAR)
          return;
      } else {
        return;
      }

      /* function calls assigned to allocatables are handled in
       * semfunc.c:func_call */
    }
  }

  sptrdest = memsym_of_ast(astdest);
  if (XBIT(54, 0x1) && !XBIT(54, 0x4) && ALLOCATTRG(sptrdest) &&
      A_TYPEG(astdest) == A_SUBSCR && DTY(dtypesrc) == TY_ARRAY &&
      DTY(dtypedest) == TY_ARRAY) {
    /* FS#21080: destination array inherits shape from source array
     * under F2003 semantics, so we can disregard empty subscripts.
     */
    int i;
    int empty_subscript;
    int asd = A_ASDG(astdest);
    int ndim = ASD_NDIM(asd);
    for (empty_subscript = i = 0; i < ndim; i++) {
      if (A_TYPEG(ASD_SUBS(asd, i)) == A_TRIPLE &&
          A_MASKG(ASD_SUBS(asd, i)) == 0x7) {
        empty_subscript = 1;
      } else {
        empty_subscript = 0;
        break;
      }
    }
    if (empty_subscript) {
      astdest = A_LOPG(astdest);
      goto again;
    }
  }

  while (A_TYPEG(astsrc) == A_CONV) {
    astsrc = A_LOPG(astsrc);
  }

  if (ALLOCATTRG(sptrdest) && A_TYPEG(astsrc) == A_INTR &&
      A_OPTYPEG(astsrc) == I_NULL) {
    ast = mk_stmt(A_ALLOC, 0);
    A_TKNP(ast, TK_DEALLOCATE);
    A_SRCP(ast, astdest);
    A_DALLOCMEMP(ast, 1);
    add_stmt_before(ast, std);
    ast_to_comment(astasgn);
    return;
  }

  if (A_TYPEG(astsrc) == A_ID || A_TYPEG(astsrc) == A_CONV ||
      A_TYPEG(astsrc) == A_SUBSCR || A_TYPEG(astsrc) == A_CNST ||
      A_TYPEG(astsrc) == A_MEM) {
    sptrsrc = memsym_of_ast(astsrc);
    if (STYPEG(sptrdest) == ST_MEMBER && STYPEG(sptrsrc) == ST_MEMBER &&
        ALLOCDESCG(sptrdest)) {
      /* FS#19589: Make sure we propagate type descriptor from source
       * to destination.
       */
      check_pointer_type(astdest, astsrc, std, 1);
    }
  }
  if (XBIT(54, 0x1) && !XBIT(54, 0x4) && sptrsrc != NOSYM &&
      (A_TYPEG(astdest) == A_ID || A_TYPEG(astdest) == A_MEM) &&
      ALLOCATTRG(sptrdest) && DTY(DTYPEG(sptrdest)) == TY_ARRAY &&
      DTY(DTYPEG(sptrsrc)) == TY_ARRAY && allocatable_member(sptrdest)
      && !has_vector_subscript_ast(astsrc)) {
    /* FS#19743: Allocate function result that's an array of derived types
     * with allocatable components and -Mallocatable=03.
     */
    /* Generate statements like this:
      if (.not. allocated(src)) then
        if (allocated(dest)) deallocate(dest)
      else
        if (.not. conformable(src, dest)) then
          if (allocated(dest) deallocate(dest)
          allocate(dest, source=src)
        end if
        poly_asn(src, dest)
      end if  <-- std2
      ...     <-- std
    */
    int std2 = std;
    if (ALLOCATTRG(sptrsrc)) {
      /* if (.not. allocated(src)) then deallocate(dest) else ... end if */
      gen_allocated_check(astsrc, std, A_IFTHEN, TRUE);
      gen_dealloc_if_allocated(astdest, std);
      add_stmt_before(mk_stmt(A_ELSE, 0), std);
      std2 = add_stmt_before(mk_stmt(A_ENDIF, 0), std);
    }
    /* if (.not. conformable(src, dst)) then */
    asttmp = mk_id(get_temp(DT_INT));
    ast = build_conformable_func_node(astdest, astsrc);
    ast = mk_assn_stmt(asttmp, ast, DT_INT);
    add_stmt_before(ast, std2);
    ast = mk_binop(OP_LT, asttmp, astb.i0, DT_INT);
    astif = mk_stmt(A_IFTHEN, 0);
    A_IFEXPRP(astif, ast);
    add_stmt_before(astif, std2);
    gen_dealloc_if_allocated(astdest, std2);
    /*   allocate(dest, source=src) */
    ast = mk_stmt(A_ALLOC, 0);
    A_TKNP(ast, TK_ALLOCATE);
    A_STARTP(ast, astsrc);
    if (DTY(dtypedest) == TY_ARRAY) {
      int src_dtype = A_DTYPEG(astsrc);
      int astdest2 =
          add_shapely_subscripts(astdest, astsrc, src_dtype, DDTG(dtypedest));
      A_SRCP(ast, astdest2);
    } else {
      A_SRCP(ast, astdest);
    }
    add_stmt_before(ast, std2);
    add_stmt_before(mk_stmt(A_ENDIF, 0), std2);

    if (STYPEG(SDSCG(sptrsrc)) == ST_MEMBER &&
        STYPEG(SDSCG(sptrdest)) == ST_MEMBER) {
      /* Generate call to poly_asn(). This call takes care of
       * the member to member assignments. This includes propagating
       * the source descriptor values to the destination descriptor.
       */
      int dest_sdsc_ast = check_member(astdest, mk_id(SDSCG(sptrdest)));
      int src_sdsc_ast = check_member(astsrc, mk_id(SDSCG(sptrsrc)));
      int fsptr = sym_mkfunc_nodesc(mkRteRtnNm(RTE_poly_asn), DT_NONE);
      int argt = mk_argt(5);
      int flag_con = mk_cval1(2, DT_INT);
      flag_con = mk_unop(OP_VAL, flag_con, DT_INT);
      ARGT_ARG(argt, 4) = flag_con;
      ARGT_ARG(argt, 0) = astdest;
      ARGT_ARG(argt, 1) = dest_sdsc_ast;
      ARGT_ARG(argt, 2) = astsrc;
      ARGT_ARG(argt, 3) = src_sdsc_ast;
      ast = mk_id(fsptr);
      ast = mk_func_node(A_CALL, ast, 5, argt);
      add_stmt_before(ast, std2);
      ast_to_comment(astasgn);
      return;
    }
  }

  /* ignore default initialization */
  if (sptrsrc > NOSYM) {
    SPTR sptr;
    if (A_TYPEG(astsrc) != A_MEM) {
      sptr = sptrsrc;
    } else if (A_TYPEG(A_PARENTG(astsrc)) == A_FUNC) {
      sptr = sym_of_ast(A_LOPG(A_PARENTG(astsrc)));
    } else {
      sptr = sym_of_ast(astsrc);
    }
    /*
     * This little bit of once-undocumented magic (formerly a string
     * comparison on the name of the RHS symbol!) forces the use of a
     * block copy for a derived type assignment whose right-hand side is
     * a compiler-created initialized prototype object used for
     * filling in new instances.  In such circumstances, the left-hand
     * side of the assignment must be assumed to be uninitialized
     * garbage.
     */
    if (sptr > NOSYM && INITIALIZERG(sptr))
      return;
  }

  /* Notes for deciphering the following code:
   *  XBIT(54, 0x1) -> enable "full F'03 allocatable attribute regularization"
   *  XBIT(54, 0x4) -> *No* 2003 allocatable assignment semantics for
   *                   allocatable components
   */
  /* Per flyspray 15461, for user-defined type assignment:
     a[i] = b , A_TYPEG(astdest) is a A_SUBSCR, also need
     to check for allocatable member.
   */
  if (!ALLOCATTRG(sptrdest) || A_TYPEG(astdest) == A_SUBSCR) {
    if (DTYG(dtypedest) == TY_DERIVED && !HCCSYMG(sptrdest) && !XBIT(54, 0x4) &&
        allocatable_member(sptrdest)) {
      handle_allocatable_members(astdest, astsrc, std, 0);
      if (!XBIT(54, 0x1) || !is_or_has_poly(sptrdest) || HCCSYMG(sptrsrc)) {
        /* pFUnit - must preserve assign if destination is not
         * allocatable, but it has polymorphic allocatable members
         */
        ast_to_comment(astasgn);
      }
      return;
    }
    if (STYPEG(sptrdest) == ST_MEMBER && !XBIT(54, 0x4) && XBIT(54, 0x1)) {
      /* FS#19118 - this typically occurs with an intrinsic assignment
       * that has a structure constructor on the right hand side. We need
       * to make sure the parent object is allocated when -Mallocatable=03
       * is used.
       */
      astdest = A_PARENTG(astdest);
      if (A_TYPEG(astdest) == A_SUBSCR)
        astdest = A_LOPG(astdest);
      if (A_TYPEG(astdest) == A_MEM) {
        sptrdest = A_SPTRG(A_MEMG(astdest));
      } else
        sptrdest = A_SPTRG(astdest);
      dtypedest = A_DTYPEG(astdest);
      if (!ALLOCATTRG(sptrdest) || DTY(dtypedest) == TY_ARRAY)
        return;
      alloc_scalar_parent_only = TRUE; /* not returning on this one path */
    } else {
      return;
    }
  }

  /*
   * The test of absence of -Mallocatable=O3 is required here ...
   */
  if (!XBIT(54, 0x1) && A_TYPEG(astdest) == A_ID && ALLOCATTRG(sptrdest) &&
      DTYG(dtypedest) == TY_DERIVED && !POINTERG(sptrdest) && !XBIT(54, 0x4) &&
      allocatable_member(sptrdest)) {
    /*
     * bug1 of f15460 -- have an allocatable array of derived type
     * containing allocatable components; with pre-F2003 semantics,
     * still must handle the allocatable components.
     */
    /*add check here too ?*/
    handle_allocatable_members(astdest, astsrc, std, 0);
    ast_to_comment(astasgn);
  }

  if (DTY(DTYPEG(sptrdest)) == TY_ARRAY && DTY(A_DTYPEG(astsrc)) != TY_ARRAY) {
    /* By definition, for
     *   array = scalar
     * the scalar has the same shape as the array.
     * Therefore, there is no need apply any allocatable
     * semantics.
     * NOTE:  CANNOT move this check before the checks for an
     * array containing allocatable components.
     */
    if (XBIT(54, 0x1) && DTYG(dtypedest) == TY_DERIVED && !POINTERG(sptrdest) &&
        !XBIT(54, 0x4) && allocatable_member(sptrdest)) {
      /* FS#18432: F2003 allocatable semantics, handle the
       * allocatable components
       */
      handle_allocatable_members(astdest, astsrc, std, 0);
      ast_to_comment(astasgn);
    }
    return;
  }

  if (!XBIT(54, 0x1) && A_TYPEG(astdest) != A_MEM) {
    if (DDTG(A_DTYPEG(astdest)) == DT_DEFERCHAR ||
        DDTG(A_DTYPEG(astdest)) == DT_DEFERNCHAR) {
      /* 03 semantics default for scalar allocatable deferred char */
      ;
    } else
      return; /* allocatable array assignment with pre F2003 semantics */
  }

  if (XBIT(54, 0x4))
    return; /* not using F'03 assignment semantics for allocatable components */

  /* move this block to a separate subroutine eventually */
  astdestparent = 0;
  if (A_TYPEG(astdest) == A_MEM) {
    astdestparent = A_PARENTG(astdest);
  }

  if (ALLOCATTRG(sptrdest) && (DTY(A_DTYPEG(astdest)) == TY_ARRAY ||
                               DTY(A_DTYPEG(astdest)) == TY_CHAR ||
                               DTY(A_DTYPEG(astdest)) == TY_NCHAR) &&
      (contains_sptr(astsrc, sptrdest, astdestparent) ||
       A_TYPEG(astsrc) == A_FUNC)) {
    int temp_ast;
    SPTR temp_sptr;
    int std2;
    int stdlast = STD_LAST;
    int shape = A_SHAPEG(astsrc);
    if (shape != 0) {
      if (DDTG(A_DTYPEG(astsrc)) == DT_DEFERCHAR ||
          DDTG(A_DTYPEG(astsrc)) == DT_DEFERNCHAR) {
        DTYPE temp_dtype = get_type(2, TY_CHAR, string_expr_length(astsrc));
        temp_dtype = dtype_with_shape(temp_dtype, shape);
        temp_sptr = get_arr_temp(temp_dtype, FALSE, FALSE);
        DTYPEP(temp_sptr, temp_dtype);
      } else {
        DTYPE temp_dtype = dtype_with_shape(dtype, shape);
        temp_sptr = get_arr_temp(temp_dtype, TRUE, TRUE);
      }
    } else if (DTY(A_DTYPEG(astdest)) == TY_CHAR ||
               DTY(A_DTYPEG(astdest)) == TY_NCHAR) {
      DTYPE temp_dtype = get_type(2, TY_CHAR, string_expr_length(astsrc));
      temp_sptr = get_ch_temp(temp_dtype);
    } else {
      /* error if it is TY_CHAR it must have shape */
      interr("transfrm: expecting shape for astsrc in assignment stmt", astasgn,
             ERR_Warning);
      goto no_lhs_on_rhs;
    }
    /*
     * NOTE - if the rhs warrants creating compiler allocatable, the
     * corresponding code will be added to the 'end' of the routine
     * since the routines being called, such as get_arr_temp(), are
     * 'semant' routines.  Therefore, the generated statements need
     * to be 'moved' to the current position.
     */
    move_stmts_before(STD_NEXT(stdlast), std);
    temp_ast = mk_id(temp_sptr);
    ast = mk_assn_stmt(temp_ast, astsrc, A_DTYPEG(astasgn));
    std2 = add_stmt_before(ast, std);
    rewrite_allocatable_assignment(ast, std2, 0);
    ast = mk_assn_stmt(astdest, temp_ast, A_DTYPEG(astasgn));
    std2 = add_stmt_after(ast, std2);
    rewrite_allocatable_assignment(ast, std2, 0);
    ast_to_comment(astasgn);
    gen_deallocate_arrays();
    move_stmts_before(STD_NEXT(stdlast), std);
    return;
  }

no_lhs_on_rhs:
  if (sptrsrc != NOSYM && ALLOCATTRG(sptrsrc)) {
    /* generate a check for an allocated source */
    gen_allocated_check(astsrc, std, A_IFTHEN, FALSE);
  }

  if (DTY(DTYPEG(sptrdest)) != TY_ARRAY) {
    /* Scalar assignment:
     * If the dest has not been allocated, then it must be.
     * Arrays will be handled based on conformability (below).
     */
    if (A_DTYPEG(astdest) == DT_DEFERCHAR ||
        A_DTYPEG(astdest) == DT_DEFERNCHAR) {
      gen_automatic_reallocation(astdest, astsrc, std);
    } else {
      int istd;
      gen_allocated_check(astdest, std, A_IFTHEN, TRUE);
      gen_alloc_mbr(build_allocation_item(0, astdest), std);
      astif = mk_stmt(A_ENDIF, 0);
      istd = add_stmt_before(astif, std);
      if (DTYG(dtypedest) == TY_DERIVED && !XBIT(54, 0x4) &&
          allocatable_member(sptrdest)) {
        nullify_member(astdest, istd, sptrdest);
      }
    }
  }

  if (alloc_scalar_parent_only) {
    goto fin;
  }

  shape = A_SHAPEG(astdest);
  if (shape != 0 && !non_conformable) {
    /* destination is array, generate conformability check */
    asttmp = mk_id(get_temp(DT_INT));
    ast = build_conformable_func_node(astdest, astsrc);
    ast = mk_assn_stmt(asttmp, ast, DT_INT);
    add_stmt_before(ast, std);

    if (DTYG(dtypedest) == TY_DERIVED) {
      /* array of derivied type
       * generate: if( tmp .eq. 1 ) then  ==> conformable
       */
      ast = mk_binop(OP_EQ, asttmp, astb.i1, DT_INT);
      astif = mk_stmt(A_IFTHEN, 0);
      A_IFEXPRP(astif, ast);
      add_stmt_before(astif, std);
    }
  }

  if (DTYG(dtypedest) == TY_DERIVED) {
    if (!XBIT(54, 0x4) && allocatable_member(sptrdest)) {
      handle_allocatable_members(astdest, astsrc, std, 0);
      ast_to_comment(astasgn);
    }
  } else if (shape != 0 && !non_conformable) {
    /* array of scalar, generate: if( tmp .le. 0 ) then => not conformable */
    ast = mk_binop(OP_LE, asttmp, astb.i0, DT_LOG);

    /* Need to check length for defer char too */
    if (DDTG(dtypedest) == DT_DEFERCHAR || DDTG(dtypedest) == DT_DEFERNCHAR) {
      int lhs_len = size_ast_of(astdest, DDTG(dtypedest));
      int rhs_len = string_expr_length(astsrc);
      int binopast = mk_binop(OP_NE, lhs_len, rhs_len, DT_LOG);
      ast = mk_binop(OP_LOR, binopast, ast, DT_LOG);
    }
    astif = mk_stmt(A_IFTHEN, 0);
    A_IFEXPRP(astif, ast);
    add_stmt_before(astif, std);
  }

  if (shape != 0) {
    if (A_TYPEG(astdest) == A_MEM) {
      shape = mk_mem_ptr_shape(A_PARENTG(astdest), A_MEMG(astdest), dtypedest);
      assert(shape != 0, "shape must not be 0", 0, ERR_Fatal);
    }

    if (DTY(dtype) == TY_ARRAY && DTY(DTY(dtype + 1)) == TY_DERIVED) {
      int destasd, srcasd;
      /*
       * in the "else" of array of derived type conformability test
       * loop over array deallocating allocatable members
       */
      int sptrmem;
      gen_allocated_check(astsrc, std, A_ELSEIF, FALSE);
      gen_allocated_check(astdest, std, A_IFTHEN, FALSE);

      /* deallocate/re-allocate array */
      gen_dealloc_mbr(astdest, std);
      astif = mk_stmt(A_ENDIF, 0); /* endif allocated dest */
      add_stmt_before(astif, std);

      gen_bounds_assignments(0, astdest, 0, astsrc, std);

      ast = build_allocation_item(0, astdest);
      gen_alloc_mbr(ast, std);

      /* loop over array re-allocating allocatable members and assigning
       * the src components to the newly alloc'd dest components */
      destasd = gen_dos_over_shape(shape, std);
      srcasd = normalize_subscripts(destasd, shape, A_SHAPEG(astsrc));
      astdestparent = subscript_allocmem(astdest, destasd);
      astsrcparent = subscript_allocmem(astsrc, srcasd);
      for (sptrmem = DTY(DDTG(dtype) + 1); sptrmem > NOSYM;
           sptrmem = SYMLKG(sptrmem)) {
        int astmem = mk_id(sptrmem);
        int astdestcmpnt = mk_member(astdestparent, astmem, A_DTYPEG(astmem));
        int astsrccmpnt = mk_member(astsrcparent, astmem, A_DTYPEG(astmem));
        if (ALLOCATTRG(sptrmem)) {
          int memshape =
              mk_mem_ptr_shape(astdestparent, astmem, A_DTYPEG(astmem));
          gen_allocated_check(astsrccmpnt, std, A_IFTHEN, FALSE);
          gen_bounds_assignments(astdestparent, astmem, astsrcparent, astmem,
                                 std);
          if (A_DTYPEG((astmem)) == DT_DEFERCHAR ||
              A_DTYPEG((astmem)) == DT_DEFERNCHAR) {
            gen_automatic_reallocation(astdestcmpnt, astsrccmpnt, std);
          } else {
            ast = build_allocation_item(astdestparent, astmem);
            gen_alloc_mbr(ast, std);
          }
          if (DTYG(DTYPEG(sptrmem)) == TY_DERIVED && !XBIT(54, 0x4) &&
              allocatable_member(sptrmem)) {
            handle_allocatable_members(astdestcmpnt, astsrccmpnt, std, 1);
          } else {
            ast = mk_assn_stmt(astdestcmpnt, astsrccmpnt, A_DTYPEG(astmem));
            add_stmt_before(ast, std);
          }
          astif = mk_stmt(A_ELSE, 0);
          add_stmt_before(astif, std);
          ast = mk_member(astdestparent, mk_id(MIDNUMG(sptrmem)),
                          DTYPEG(MIDNUMG(sptrmem)));
          {
            int aa;
            aa = begin_call(A_ICALL, intast_sym[I_NULLIFY], 1);
            A_OPTYPEP(aa, I_NULLIFY);
            add_arg(ast);
            ast = aa;
          }
          add_stmt_before(ast, std);
          astif = mk_stmt(A_ENDIF, 0);
          add_stmt_before(astif, std);
        } else if (POINTERG(sptrmem) && !F90POINTERG(sptrmem)) {
          astsrccmpnt = mk_member(astsrcparent, astmem, A_DTYPEG(astmem));
          ast = add_ptr_assign(astdestcmpnt, astsrccmpnt, std);
          A_SHAPEP(ast, A_SHAPEG(astsrccmpnt));
          add_stmt_before(ast, std);
        } else if (DTYG(DTYPEG(sptrmem)) == TY_DERIVED && !XBIT(54, 0x4) &&
                   allocatable_member(sptrmem)) {
          handle_allocatable_members(astdestcmpnt, astsrccmpnt, std, 1);
        } else {
          astsrccmpnt = mk_member(astsrcparent, astmem, A_DTYPEG(astmem));
          ast = mk_assn_stmt(astdestcmpnt, astsrccmpnt, A_DTYPEG(astmem));
          add_stmt_before(ast, std);
        }

        if (ALLOCG(sptrmem) || (POINTERG(sptrmem) && !F90POINTERG(sptrmem))) {
          sptrmem = SDSCG(sptrmem); /* set-up to move past $p, $o, $sd */
        }
      }
      gen_do_ends(ASD_NDIM(destasd), std);
    } else {
      /* in the "not conformable" path of conformability check for allocatable
       * array of intrinsic type, generate:
       *   rewrite_deallocate(dest)
       *   allocate(dest(lb(src): ub(src)))
       * endif  */
      int astmem;
      int astsrcmem;

      if (!non_conformable) {
        gen_dealloc_mbr(astdest, std);
      }
      if (A_TYPEG(astdest) == A_MEM) {
        astdestparent = A_PARENTG(astdest);
        astmem = A_MEMG(astdest);
      } else {
        astdestparent = 0;
        astmem = astdest;
      }
      if (A_TYPEG(astsrc) == A_MEM) {
        astsrcparent = A_PARENTG(astsrc);
        astsrcmem = A_MEMG(astsrc);
      } else {
        astsrcparent = 0;
        astsrcmem = astsrc;
      }
      gen_bounds_assignments(astdestparent, astmem, astsrcparent, astsrcmem,
                             std);
      ast = build_allocation_item(astdestparent, astmem);
      gen_alloc_mbr(ast, std);
    }
    if (!non_conformable) {
      astif = mk_stmt(A_ENDIF, 0);
      add_stmt_before(astif, std);
    }
  }
fin:
  if (sptrsrc != NOSYM && ALLOCATTRG(sptrsrc)) {
    /* !allocated(src)), generate
     * else if( allocated(dest))
     *   rewrite_deallocate(dest)
     * endif
     */
    gen_allocated_check(astdest, stdnext, A_ELSEIF, FALSE);
    gen_dealloc_mbr(astdest, stdnext);
    astif = mk_stmt(A_ENDIF, 0);
    add_stmt_before(astif, stdnext);
  }
}

/* if (allocated(ast)) deallocate(ast) */
static void
gen_dealloc_if_allocated(int ast, int std)
{
  int alloc_ast = mk_stmt(A_ALLOC, 0);
  gen_allocated_check(ast, std, A_IFTHEN, FALSE);
  A_TKNP(alloc_ast, TK_DEALLOCATE);
  A_SRCP(alloc_ast, ast);
  add_stmt_before(alloc_ast, std);
  add_stmt_before(mk_stmt(A_ENDIF, 0), std);
}

static void
find_allocatable_assignment(void)
{
  int std;
  int stdnext;
  int workshare_depth;

  sem.sc = SC_LOCAL;
  workshare_depth = 0;
  for (std = STD_NEXT(0); std != 0; std = stdnext) {
    int ast;
    int match;

    ast = STD_AST(std);
    stdnext = STD_NEXT(std);
    switch (A_TYPEG(ast)) {
    case A_MP_PARALLEL:
    case A_MP_TASK:
      A_OPT1P(ast, sem.sc);
      sem.sc = SC_PRIVATE;
      break;
    case A_MP_ENDPARALLEL:
    case A_MP_ENDTASK:
      match = A_LOPG(ast);
      sem.sc = A_OPT1G(match);
      A_OPT1P(match, 0);
      break;
    case A_MP_WORKSHARE:
      workshare_depth++;
      break;
    case A_MP_ENDWORKSHARE:
      workshare_depth--;
      break;
    case A_ASN:
      if (!workshare_depth &&
          (A_TYPEG(A_DESTG(ast)) != A_SUBSCR
           /* Per flyspray 15461, for user-defined type assignment:
              a[i] = b , A_TYPEG(A_DESTG(ast)) is a A_SUBSCR, also need
              to check for allocatable member if it is user-defined type.
            */
           || DTYG(A_DTYPEG(A_DESTG(ast))) == TY_DERIVED)) {
        rewrite_allocatable_assignment(ast, std, 0);
      }
      break;
    }
  }
}

/* Create new asd from subscripts in oldasd by normalizing from oldshape to
   newshape. */
static int
normalize_subscripts(int oldasd, int oldshape, int newshape)
{
  int i;
  int newsubs[MAXSUBS];
  int ndim = SHD_NDIM(oldshape);

  assert(ndim == ASD_NDIM(oldasd), "ndim does not match", ndim, ERR_Fatal);
  for (i = 0; i < ndim; i++) {
    int oldsub = ASD_SUBS(oldasd, i);
    newsubs[i] = normalize_subscript(
        oldsub, SHD_LWB(oldshape, i), SHD_STRIDE(oldshape, i),
        SHD_LWB(newshape, i), SHD_STRIDE(newshape, i));
  }
  return mk_asd(newsubs, ndim);
}

/* aref represents a reference to an allocatable component where its parent
 * has shape. asd represents subscripts to be applied.
 * Need to recurse through the parent to find the correct object
 * to which the subscripts are applied.  After the subscripting has been
 * done, need to (re)apply the member and the subscript references which we
 * had recursed.
 */
static int
subscript_allocmem(int aref, int asd)
{
  int ndim = ASD_NDIM(asd);
  int subs[MAXSUBS];

  switch (A_TYPEG(aref)) {
  case A_SUBSCR: {
    int asd2 = A_ASDG(aref);
    int n = ASD_NDIM(asd2);
    int ast, i, vector;
    for (i = 0, vector = 0; i < n; ++i) {
      int sub = ASD_SUBS(asd2, i);
      if (DTY(A_DTYPEG(sub)) == TY_ARRAY) {
        int tmp = ASD_SUBS(asd, vector);
        int subasd = mk_asd(&tmp, 1);
        if (A_TYPEG(sub) == A_SUBSCR) {
          sub = subscript_allocmem(sub, subasd);
        } else {
          sub = mk_subscr_copy(sub, subasd, DTY(A_DTYPEG(sub) + 1));
        }
        vector++;
      } else if (A_TYPEG(sub) == A_TRIPLE) {
        sub = ASD_SUBS(asd, vector);
        vector++;
      }
      subs[i] = sub;
    }
    ast = A_LOPG(aref);
    if (vector == 0) {
      ast = subscript_allocmem(ast, asd);
    }
    return mk_subscr(ast, subs, n, A_DTYPEG(aref));
  }
  case A_MEM:
    if (vector_member(aref)) {
      return mk_subscr_copy(aref, asd, DTY(A_DTYPEG(aref) + 1));
    } else {
      int ast = subscript_allocmem(A_PARENTG(aref), asd);
      return mk_member(ast, A_MEMG(aref), A_DTYPEG(A_MEMG(aref)));
    }
  case A_ID:
    assert(DTY(A_DTYPEG(aref)) == TY_ARRAY, "subscript_allocmem: not array", 0,
           4);
    return mk_subscr_copy(aref, asd, DTY(A_DTYPEG(aref) + 1));
  default:
    interr("subscript_allocmem: bad ast type", A_TYPEG(aref), ERR_Fatal);
    return 0;
  }
}