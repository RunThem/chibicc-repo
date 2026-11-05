// 本文件把标注树变成 x86-64 汇编, 同时承载 shaping pass - 旧流水线中把
// 忠实树改写成发射器所理解形状的那部分. 这个 pass 对每个函数运行一次,
// 在分配或发射任何东西之前 (plans/PLAN.md 的 codegen-direct 路线把它
// 的各部分逐步搬到这里), 并遵守六条不变量 (plan contract 3):
//
//  (a) 后序: 节点的子树先于该节点被改写 - 外层改写可以依赖内层改写;
//  (b) 阶段顺序: 所有表达式与声明 shaping 都在控制流标签分配之前完成,
//      因此第一阶段构造的循环仍能拿到自己的标签;
//  (c) 每函数上下文: 改写创建的临时槽追加到当前函数的局部变量上,
//      且每个槽都在 assign_lvar_offsets 运行之前就已存在;
//  (d) 不重入: pass 构造的子树在构造时即已降级, 且 pass 从不再次遍历
//      自己改写过的子树 - 树中不存在能把它引回去的别名;
//  (e) 只访问一次: 嵌套函数体只从程序列表 shaping 恰好一次, 绝不通过
//      宿主函数体抵达;
//  (f) 初始化器不在自然路径上: 已 resolve 的初始化器树由 shape_init_exprs
//      显式遍历, 绝不通过下降节点的字段抵达.
//
// 加法运算符是发射器自己执行的唯一一处降级: 忠实的 `+`/`-` 到达时
// 未做缩放, gen_expr 在发射它之前把节点替换为 new_add/new_sub 的产物
// (PLAN B2). 这两个构造函数建出的节点带 `is_lowered` 标记, 发射器据此
// 把已降级的节点 - gen_addr 构造的 `x[y]` 地址, 复合赋值已缩放的运算数
// - 与忠实的节点区分开.
//
// 这个 pass 还掌管标签命名空间与临时槽 (返回缓冲区, VLA 尺寸槽等);
// 它们保存在 codegen 私有的 per-node / per-type 表中 (见上文, PLAN L1).
// 以下全部是发射器: 按设计没有优化 pass.

#include "driver.h"

#define GP_MAX 6
#define FP_MAX 8

static FILE* output_file;
static int depth;
static char* argreg8[]  = {"%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b"};
static char* argreg16[] = {"%di", "%si", "%dx", "%cx", "%r8w", "%r9w"};
static char* argreg32[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
static char* argreg64[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
static LibcpObj* current_fn;

// 以前放在公共 LibcpNode/LibcpType/LibcpObj 结构体上的 codegen 私有状态
// (PLAN L1). 面向库的形状只保留源码与 sema 放上去的东西; 标签, 降级标记,
// 调用槽和 VLA 尺寸槽在这里以指针为键. 条目惰性创建, 存活到进程结束,
// 与本消费者的其他分配一样.
typedef struct CgNode CgNode;
struct CgNode {
  LibcpNode* node;
  CgNode* next;
  char* brk_label;
  char* cont_label;
  char* unique_label;
  LibcpObj* ret_buffer;
  LibcpNode* case_next;
  LibcpNode* default_case;
  bool pass_by_stack;
  bool is_lowered;
};

typedef struct CgType CgType;
struct CgType {
  LibcpType* ty;
  CgType* next;
  LibcpObj* vla_size;
};

typedef struct CgObj CgObj;
struct CgObj {
  LibcpObj* obj;
  CgObj* next;
  int offset;
  int stack_size;
};

#define CG_BUCKETS 4096
static CgNode* cg_nodes[CG_BUCKETS];
static CgType* cg_types[CG_BUCKETS];
static CgObj* cg_objs[CG_BUCKETS];

static unsigned cg_hash(void* p) {
  uintptr_t x = (uintptr_t)p;
  x ^= x >> 17;
  x *= (uintptr_t)0xed5ad4bbU;
  x ^= x >> 11;
  return x % CG_BUCKETS;
}

static CgNode* cg(LibcpNode* node) {
  unsigned h = cg_hash(node);
  for (CgNode* p = cg_nodes[h]; p; p = p->next)
    if (p->node == node)
      return p;

  CgNode* p   = calloc(1, sizeof(CgNode));
  p->node     = node;
  p->next     = cg_nodes[h];
  cg_nodes[h] = p;
  return p;
}

static CgType* cg_type(LibcpType* ty) {
  unsigned h = cg_hash(ty);
  for (CgType* p = cg_types[h]; p; p = p->next)
    if (p->ty == ty)
      return p;

  CgType* p   = calloc(1, sizeof(CgType));
  p->ty       = ty;
  p->next     = cg_types[h];
  cg_types[h] = p;
  return p;
}

static CgObj* cg_obj(LibcpObj* obj) {
  unsigned h = cg_hash(obj);
  for (CgObj* p = cg_objs[h]; p; p = p->next)
    if (p->obj == obj)
      return p;

  CgObj* p   = calloc(1, sizeof(CgObj));
  p->obj     = obj;
  p->next    = cg_objs[h];
  cg_objs[h] = p;
  return p;
}

static int obj_offset(LibcpObj* obj) {
  return cg_obj(obj)->offset;
}

static int obj_stack_size(LibcpObj* obj) {
  return cg_obj(obj)->stack_size;
}

// 返回低 `width` 位全置 1 的掩码. 全宽位域若按 width 移位就是 UB (且
// x86 会对计数取模, 得到零掩码), 所以这种情况单独写出.
static unsigned long bitfield_mask(int width) {
  return width == 64 ? ~0UL : (1UL << width) - 1;
}

// 发射函数体期间最内层包围循环的 break 与 continue 标签 (PLAN B1.2a):
// break 或 continue 在这里对照它们解析, 而旧实现中由 shaping pass 把标签
// 绑进节点. switch 只保存 break 标签 - 它是 break 目标, 但不是 continue
// 目标.
static char* cur_brk_label;
static char* cur_cont_label;

static void gen_expr(LibcpContext* ctx, LibcpNode* node);
static void gen_stmt(LibcpContext* ctx, LibcpNode* node);
static LibcpNode* new_add(LibcpContext* ctx, LibcpNode* lhs, LibcpNode* rhs, LibcpToken* tok);
static LibcpNode* new_sub(LibcpContext* ctx, LibcpNode* lhs, LibcpNode* rhs, LibcpToken* tok);
static LibcpNode*
    lvar_init_comma(LibcpContext* ctx, LibcpObj* var, LibcpInitTree* init, LibcpToken* tok);

__attribute__((format(printf, 1, 2))) static void println(char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(output_file, fmt, ap);
  va_end(ap);
  fprintf(output_file, "\n");
}

static int count(void) {
  static int i = 1;
  return i++;
}

static void push(void) {
  println("  push %%rax");
  depth++;
}

static void pop(char* arg) {
  println("  pop %s", arg);
  depth--;
}

static void pushf(void) {
  println("  sub $8, %%rsp");
  println("  movsd %%xmm0, (%%rsp)");
  depth++;
}

static void popf(int reg) {
  println("  movsd (%%rsp), %%xmm%d", reg);
  println("  add $8, %%rsp");
  depth--;
}

// 把 `n` 向上取整到 `align` 最近倍数的函数现在位于 strings.c: sema 也
// 需要它, 而 codegen 不能成为库的依赖.

static char* reg_dx(LibcpContext* ctx, int sz) {
  switch (sz) {
    case 1: return "%dl";
    case 2: return "%dx";
    case 4: return "%edx";
    case 8: return "%rdx";
  }
  unreachable();
}

static char* reg_ax(LibcpContext* ctx, int sz) {
  switch (sz) {
    case 1: return "%al";
    case 2: return "%ax";
    case 4: return "%eax";
    case 8: return "%rax";
  }
  unreachable();
}

// 计算给定节点的绝对地址.
// 若节点不位于内存中则报错.
static void gen_addr(LibcpContext* ctx, LibcpNode* node) {
  switch (node->kind) {
    case ND_STRING:
      // 字面量的匿名全局对象 (PLAN A9.1: 节点保留自己的 kind), 与其他
      // 全局对象无异 - 下面的 ND_VAR 处理通过同一个 `var` 字段覆盖它.
    case ND_VAR:
      // 变长数组, 总是局部变量.
      if (node->var->ty->kind == TY_VLA) {
        println("  mov %d(%%rbp), %%rax", obj_offset(node->var));
        return;
      }

      // 局部变量
      if (node->var->is_local) {
        println("  lea %d(%%rbp), %%rax", obj_offset(node->var));
        return;
      }

      if (opt_fpic) {
        // 线程局部变量
        if (node->var->is_tls) {
          println("  data16 lea %s@tlsgd(%%rip), %%rdi", node->var->name);
          println("  .value 0x6666");
          println("  rex64");
          println("  call __tls_get_addr@PLT");
          return;
        }

        // 函数或全局变量
        println("  mov %s@GOTPCREL(%%rip), %%rax", node->var->name);
        return;
      }

      // 线程局部变量
      if (node->var->is_tls) {
        println("  mov %%fs:0, %%rax");
        println("  add $%s@tpoff, %%rax", node->var->name);
        return;
      }

      // 这里生成函数或全局变量的绝对地址. 它们在运行时有确定地址, 但
      // 链接期并不知道, 原因有二.
      //
      //  - 地址随机化: 可执行文件整体加载到内存, 但加载到哪个地址并不
      //    得知. 因此在链接期, 同一可执行文件内的相对地址 (即同一可执行
      //    文件中两个函数之间的距离) 是已知的, 绝对地址则未知.
      //
      //  - 动态链接: 动态共享对象 (DSO) 或 .so 文件在运行时与可执行文件
      //    一起加载到内存, 由运行时加载器在内存中链接. 在运行时重定位
      //    完成之前, 对 DSO 可能定义的全局对象地址一无所知.
      //
      // 对前一种情况使用 RIP 相对寻址, 记作 `(%rip)`. 对后一种情况,
      // 通过 `@GOTPCREL(%rip)` 记法从全局偏移表 (GOT) 取得可能位于共享
      // 对象文件中的对象地址.

      // 函数
      if (node->ty->kind == TY_FUNC) {
        if (node->var->is_definition)
          println("  lea %s(%%rip), %%rax", node->var->name);
        else
          println("  mov %s@GOTPCREL(%%rip), %%rax", node->var->name);
        return;
      }

      // 全局变量
      println("  lea %s(%%rip), %%rax", node->var->name);
      return;
    case ND_DEREF: gen_expr(ctx, node->lhs); return;
    case ND_COMMA:
      gen_expr(ctx, node->lhs);
      gen_addr(ctx, node->rhs);
      return;
    case ND_SUBSCRIPT: {
      // `x[y]` 保持忠实写法 (PLAN B1.1a): 以前由 shaping pass 预先构造的
      // 已缩放, 已转换的和, 改在这里构造 - 就在需要地址的时刻 - 并立即
      // 发射. new_add 为它定类型, 并完全按那个 pass 的做法施加元素大小
      // 缩放, 因此指令与旧 `*(x+y)` 形式产生的一样. 它带降级标记, 所以
      // 发射器自己的加法降级 (PLAN B2) 不会碰它.
      LibcpNode* add = new_add(ctx, node->lhs, node->rhs, node->tok);
      gen_expr(ctx, add);
      return;
    }
    case ND_MEMBER:
      // 仍带 `->` 标记的成员访问 (PLAN B1.1b) 走其运算数本身的指针值,
      // 因此它贡献的地址就是这个值本身; 普通的 `.` 形式仍取运算数的
      // 地址. 成员绑定与成员类型已在 sema 中确定.
      if (node->arrow_tok)
        gen_expr(ctx, node->lhs);
      else
        gen_addr(ctx, node->lhs);
      println("  add $%d, %%rax", node->member->offset);
      return;
    case ND_FUNCALL:
      if (cg(node)->ret_buffer) {
        gen_expr(ctx, node);
        return;
      }
      break;
    case ND_GENERIC:
      // 选择表达式的结论: 若被选中的结果本身是左值, 它也是左值
      // (PLAN A9.1: 节点保留自己的 kind, 结果由字段携带).
      gen_addr(ctx, node->generic_sel);
      return;
    case ND_ASSIGN:
    case ND_COND:
      if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) {
        gen_expr(ctx, node);
        return;
      }
      break;
    case ND_VLA_PTR: println("  lea %d(%%rbp), %%rax", obj_offset(node->var)); return;
  }

  libcp_error_tok(ctx, node->tok, "not an lvalue");
}

// 从 %rax 指向的位置加载值.
static void load(LibcpType* ty) {
  switch (ty->kind) {
    case TY_ARRAY:
    case TY_STRUCT:
    case TY_UNION:
    case TY_FUNC:
    case TY_VLA:
      // 若是数组, 不要试图把值加载到寄存器, 因为通常无法把整个数组装入
      // 寄存器. 因此数组求值的结果不是数组本身, 而是数组的地址. C 中
      // "数组自动转换为指向其首元素的指针" 就发生在这里.
      return;
    case TY_FLOAT: println("  movss (%%rax), %%xmm0"); return;
    case TY_DOUBLE: println("  movsd (%%rax), %%xmm0"); return;
    case TY_LDOUBLE: println("  fldt (%%rax)"); return;
  }

  char* insn = ty->is_unsigned ? "movz" : "movs";

  // 把 char 或 short 值加载到寄存器时总是扩展到 int 大小, 因此可以假定
  // 寄存器的低半部分总是含有有效值. char, short 和 int 对应寄存器的高半
  // 部分可能含有垃圾. 把 long 值加载到寄存器时, 它直接占用整个寄存器.
  if (ty->size == 1)
    println("  %sbl (%%rax), %%eax", insn);
  else if (ty->size == 2)
    println("  %swl (%%rax), %%eax", insn);
  else if (ty->size == 4)
    println("  movsxd (%%rax), %%rax");
  else
    println("  mov (%%rax), %%rax");
}

// 把 %rax 存入栈顶所指向的地址.
static void store(LibcpType* ty) {
  pop("%rdi");

  switch (ty->kind) {
    case TY_STRUCT:
    case TY_UNION:
      for (int i = 0; i < ty->size; i++) {
        println("  mov %d(%%rax), %%r8b", i);
        println("  mov %%r8b, %d(%%rdi)", i);
      }
      return;
    case TY_FLOAT: println("  movss %%xmm0, (%%rdi)"); return;
    case TY_DOUBLE: println("  movsd %%xmm0, (%%rdi)"); return;
    case TY_LDOUBLE: println("  fstpt (%%rdi)"); return;
  }

  if (ty->size == 1)
    println("  mov %%al, (%%rdi)");
  else if (ty->size == 2)
    println("  mov %%ax, (%%rdi)");
  else if (ty->size == 4)
    println("  mov %%eax, (%%rdi)");
  else
    println("  mov %%rax, (%%rdi)");
}

static void cmp_zero(LibcpType* ty) {
  switch (ty->kind) {
    case TY_FLOAT:
      println("  xorps %%xmm1, %%xmm1");
      println("  ucomiss %%xmm1, %%xmm0");
      return;
    case TY_DOUBLE:
      println("  xorpd %%xmm1, %%xmm1");
      println("  ucomisd %%xmm1, %%xmm0");
      return;
    case TY_LDOUBLE:
      println("  fldz");
      println("  fucomip");
      println("  fstp %%st(0)");
      return;
  }

  if (libcp_is_integer(ty) && ty->size <= 4)
    println("  cmp $0, %%eax");
  else
    println("  cmp $0, %%rax");
}

enum {
  I8,
  I16,
  I32,
  I64,
  U8,
  U16,
  U32,
  U64,
  F32,
  F64,
  F80
};

static int getTypeId(LibcpType* ty) {
  switch (ty->kind) {
    case TY_CHAR: return ty->is_unsigned ? U8 : I8;
    case TY_SHORT: return ty->is_unsigned ? U16 : I16;
    case TY_INT: return ty->is_unsigned ? U32 : I32;
    case TY_LONG: return ty->is_unsigned ? U64 : I64;
    case TY_FLOAT: return F32;
    case TY_DOUBLE: return F64;
    case TY_LDOUBLE: return F80;
  }
  return U64;
}

// 类型转换表
static char i32i8[]  = "movsbl %al, %eax";
static char i32u8[]  = "movzbl %al, %eax";
static char i32i16[] = "movswl %ax, %eax";
static char i32u16[] = "movzwl %ax, %eax";
static char i32f32[] = "cvtsi2ssl %eax, %xmm0";
static char i32i64[] = "movsxd %eax, %rax";
static char i32f64[] = "cvtsi2sdl %eax, %xmm0";
static char i32f80[] = "mov %eax, -4(%rsp); fildl -4(%rsp)";

static char u32f32[] = "mov %eax, %eax; cvtsi2ssq %rax, %xmm0";
static char u32i64[] = "mov %eax, %eax";
static char u32f64[] = "mov %eax, %eax; cvtsi2sdq %rax, %xmm0";
static char u32f80[] = "mov %eax, %eax; mov %rax, -8(%rsp); fildll -8(%rsp)";

static char i64f32[] = "cvtsi2ssq %rax, %xmm0";
static char i64f64[] = "cvtsi2sdq %rax, %xmm0";
static char i64f80[] = "movq %rax, -8(%rsp); fildll -8(%rsp)";

static char u64f32[] = "cvtsi2ssq %rax, %xmm0";
static char u64f64[] = "test %rax,%rax; js 1f; pxor %xmm0,%xmm0; cvtsi2sd %rax,%xmm0; jmp 2f; "
                       "1: mov %rax,%rdi; and $1,%eax; pxor %xmm0,%xmm0; shr %rdi; "
                       "or %rax,%rdi; cvtsi2sd %rdi,%xmm0; addsd %xmm0,%xmm0; 2:";
static char u64f80[] = "mov %rax, -8(%rsp); fildq -8(%rsp); test %rax, %rax; jns 1f;"
                       "mov $1602224128, %eax; mov %eax, -4(%rsp); fadds -4(%rsp); 1:";

static char f32i8[]  = "cvttss2sil %xmm0, %eax; movsbl %al, %eax";
static char f32u8[]  = "cvttss2sil %xmm0, %eax; movzbl %al, %eax";
static char f32i16[] = "cvttss2sil %xmm0, %eax; movswl %ax, %eax";
static char f32u16[] = "cvttss2sil %xmm0, %eax; movzwl %ax, %eax";
static char f32i32[] = "cvttss2sil %xmm0, %eax";
static char f32u32[] = "cvttss2siq %xmm0, %rax";
static char f32i64[] = "cvttss2siq %xmm0, %rax";
static char f32u64[] = "cvttss2siq %xmm0, %rax";
static char f32f64[] = "cvtss2sd %xmm0, %xmm0";
static char f32f80[] = "movss %xmm0, -4(%rsp); flds -4(%rsp)";

static char f64i8[]  = "cvttsd2sil %xmm0, %eax; movsbl %al, %eax";
static char f64u8[]  = "cvttsd2sil %xmm0, %eax; movzbl %al, %eax";
static char f64i16[] = "cvttsd2sil %xmm0, %eax; movswl %ax, %eax";
static char f64u16[] = "cvttsd2sil %xmm0, %eax; movzwl %ax, %eax";
static char f64i32[] = "cvttsd2sil %xmm0, %eax";
static char f64u32[] = "cvttsd2siq %xmm0, %rax";
static char f64i64[] = "cvttsd2siq %xmm0, %rax";
static char f64u64[] = "cvttsd2siq %xmm0, %rax";
static char f64f32[] = "cvtsd2ss %xmm0, %xmm0";
static char f64f80[] = "movsd %xmm0, -8(%rsp); fldl -8(%rsp)";

#define FROM_F80_1                                                                                 \
  "fnstcw -10(%rsp); movzwl -10(%rsp), %eax; or $12, %ah; "                                        \
  "mov %ax, -12(%rsp); fldcw -12(%rsp); "

#define FROM_F80_2 " -24(%rsp); fldcw -10(%rsp); "

static char f80i8[]  = FROM_F80_1 "fistps" FROM_F80_2 "movsbl -24(%rsp), %eax";
static char f80u8[]  = FROM_F80_1 "fistps" FROM_F80_2 "movzbl -24(%rsp), %eax";
static char f80i16[] = FROM_F80_1 "fistps" FROM_F80_2 "movzbl -24(%rsp), %eax";
static char f80u16[] = FROM_F80_1 "fistpl" FROM_F80_2 "movswl -24(%rsp), %eax";
static char f80i32[] = FROM_F80_1 "fistpl" FROM_F80_2 "mov -24(%rsp), %eax";
static char f80u32[] = FROM_F80_1 "fistpl" FROM_F80_2 "mov -24(%rsp), %eax";
static char f80i64[] = FROM_F80_1 "fistpq" FROM_F80_2 "mov -24(%rsp), %rax";
static char f80u64[] = FROM_F80_1 "fistpq" FROM_F80_2 "mov -24(%rsp), %rax";
static char f80f32[] = "fstps -8(%rsp); movss -8(%rsp), %xmm0";
static char f80f64[] = "fstpl -8(%rsp); movsd -8(%rsp), %xmm0";

static char* cast_table[][11] = {
    // i8   i16     i32     i64     u8     u16     u32     u64     f32     f64     f80
    {NULL,  NULL,   NULL,   i32i64, i32u8, i32u16, NULL,   i32i64, i32f32, i32f64, i32f80}, // i8
    {i32i8, NULL,   NULL,   i32i64, i32u8, i32u16, NULL,   i32i64, i32f32, i32f64, i32f80}, // i16
    {i32i8, i32i16, NULL,   i32i64, i32u8, i32u16, NULL,   i32i64, i32f32, i32f64, i32f80}, // i32
    {i32i8, i32i16, NULL,   NULL,   i32u8, i32u16, NULL,   NULL,   i64f32, i64f64, i64f80}, // i64

    {i32i8, NULL,   NULL,   i32i64, NULL,  NULL,   NULL,   i32i64, i32f32, i32f64, i32f80}, // u8
    {i32i8, i32i16, NULL,   i32i64, i32u8, NULL,   NULL,   i32i64, i32f32, i32f64, i32f80}, // u16
    {i32i8, i32i16, NULL,   u32i64, i32u8, i32u16, NULL,   u32i64, u32f32, u32f64, u32f80}, // u32
    {i32i8, i32i16, NULL,   NULL,   i32u8, i32u16, NULL,   NULL,   u64f32, u64f64, u64f80}, // u64

    {f32i8, f32i16, f32i32, f32i64, f32u8, f32u16, f32u32, f32u64, NULL,   f32f64, f32f80}, // f32
    {f64i8, f64i16, f64i32, f64i64, f64u8, f64u16, f64u32, f64u64, f64f32, NULL,   f64f80}, // f64
    {f80i8, f80i16, f80i32, f80i64, f80u8, f80u16, f80u32, f80u64, f80f32, f80f64, NULL  }, // f80
};

static void cast(LibcpType* from, LibcpType* to) {
  if (to->kind == TY_VOID)
    return;

  if (to->kind == TY_BOOL) {
    cmp_zero(from);
    println("  setne %%al");
    println("  movzx %%al, %%eax");
    return;
  }

  int t1 = getTypeId(from);
  int t2 = getTypeId(to);
  if (cast_table[t1][t2])
    println("  %s", cast_table[t1][t2]);
}

// 大小等于或小于 16 字节的 struct 或 union 最多用两个寄存器传递.
//
// 若前 8 个字节只含浮点类型成员, 则用 XMM 寄存器传递; 否则用通用寄存器
// 传递.
//
// 若 struct/union 大于 8 字节, 对下一个 8 字节块套用同样的规则.
//
// 若 `ty` 在其字节区间 [lo, hi) 内只含浮点成员, 本函数返回 true.
static bool has_flonum(LibcpType* ty, int lo, int hi, int offset) {
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    for (LibcpMember* mem = ty->members; mem; mem = mem->next)
      if (!has_flonum(mem->ty, lo, hi, offset + mem->offset))
        return false;
    return true;
  }

  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < ty->array_len; i++)
      if (!has_flonum(ty->base, lo, hi, offset + ty->base->size * i))
        return false;
    return true;
  }

  return offset < lo || hi <= offset || ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE;
}

static bool has_flonum1(LibcpType* ty) {
  return has_flonum(ty, 0, 8, 0);
}

static bool has_flonum2(LibcpType* ty) {
  return has_flonum(ty, 8, 16, 0);
}

static void push_struct(LibcpType* ty) {
  int sz = libcp_align_to(ty->size, 8);
  println("  sub $%d, %%rsp", sz);
  depth += sz / 8;

  for (int i = 0; i < ty->size; i++) {
    println("  mov %d(%%rax), %%r10b", i);
    println("  mov %%r10b, %d(%%rsp)", i);
  }
}

static void push_args2(LibcpContext* ctx, LibcpNode* args, bool first_pass) {
  if (!args)
    return;
  push_args2(ctx, args->next, first_pass);

  CgNode* ci = cg(args);
  if ((first_pass && !ci->pass_by_stack) || (!first_pass && ci->pass_by_stack))
    return;

  gen_expr(ctx, args);

  switch (args->ty->kind) {
    case TY_STRUCT:
    case TY_UNION: push_struct(args->ty); break;
    case TY_FLOAT:
    case TY_DOUBLE: pushf(); break;
    case TY_LDOUBLE:
      println("  sub $16, %%rsp");
      println("  fstpt (%%rsp)");
      depth += 2;
      break;
    default: push();
  }
}

// 为函数调用安排实参传递. 这里按 x86-64 psABI 决定每个实参走寄存器
// 还是走栈, 并求值后压栈; 调用点随后把走寄存器的实参弹入对应寄存器.
// 规范的规定是:
//
// - 整型实参最多 6 个, 用 RDI, RSI, RDX, RCX, R8 和 R9 传递.
//
// - 浮点类型实参最多 8 个, 用 XMM0 到 XMM7 传递.
//
// - 相应类型的寄存器已经用完时, 实参按从右到左的顺序压栈.
//
// - 每个压栈传递的实参占 8 字节, 实参区域的末端必须对齐到 16 字节边界.
//
// - 函数是可变参数时, 把浮点类型实参的个数写入 RAX.
static int push_args(LibcpContext* ctx, LibcpNode* node) {
  int stack = 0, gp = 0, fp = 0;

  // 返回类型是大的 struct/union 时, 调用方像传第一个实参那样传入一个
  // 缓冲区指针.
  if (cg(node)->ret_buffer && node->ty->size > 16)
    gp++;

  // 尽可能让实参走寄存器.
  for (LibcpNode* arg = node->args; arg; arg = arg->next) {
    LibcpType* ty = arg->ty;

    switch (ty->kind) {
      case TY_STRUCT:
      case TY_UNION:
        if (ty->size > 16) {
          cg(arg)->pass_by_stack = true;
          stack += libcp_align_to(ty->size, 8) / 8;
        } else {
          bool fp1 = has_flonum1(ty);
          bool fp2 = has_flonum2(ty);

          if (fp + fp1 + fp2 < FP_MAX && gp + !fp1 + !fp2 < GP_MAX) {
            fp = fp + fp1 + fp2;
            gp = gp + !fp1 + !fp2;
          } else {
            cg(arg)->pass_by_stack = true;
            stack += libcp_align_to(ty->size, 8) / 8;
          }
        }
        break;
      case TY_FLOAT:
      case TY_DOUBLE:
        if (fp++ >= FP_MAX) {
          cg(arg)->pass_by_stack = true;
          stack++;
        }
        break;
      case TY_LDOUBLE:
        cg(arg)->pass_by_stack = true;
        stack += 2;
        break;
      default:
        if (gp++ >= GP_MAX) {
          cg(arg)->pass_by_stack = true;
          stack++;
        }
    }
  }

  if ((depth + stack) % 2 == 1) {
    println("  sub $8, %%rsp");
    depth++;
    stack++;
  }

  push_args2(ctx, node->args, true);
  push_args2(ctx, node->args, false);

  // 返回类型是大的 struct/union 时, 调用方像传第一个实参那样传入一个
  // 缓冲区指针.
  if (cg(node)->ret_buffer && node->ty->size > 16) {
    println("  lea %d(%%rbp), %%rax", obj_offset(cg(node)->ret_buffer));
    push();
  }

  return stack;
}

static void copy_ret_buffer(LibcpObj* var) {
  LibcpType* ty = var->ty;
  int gp = 0, fp = 0;

  if (has_flonum1(ty)) {
    assert(ty->size == 4 || 8 <= ty->size);
    if (ty->size == 4)
      println("  movss %%xmm0, %d(%%rbp)", obj_offset(var));
    else
      println("  movsd %%xmm0, %d(%%rbp)", obj_offset(var));
    fp++;
  } else {
    for (int i = 0; i < MIN(8, ty->size); i++) {
      println("  mov %%al, %d(%%rbp)", obj_offset(var) + i);
      println("  shr $8, %%rax");
    }
    gp++;
  }

  if (ty->size > 8) {
    if (has_flonum2(ty)) {
      assert(ty->size == 12 || ty->size == 16);
      if (ty->size == 12)
        println("  movss %%xmm%d, %d(%%rbp)", fp, obj_offset(var) + 8);
      else
        println("  movsd %%xmm%d, %d(%%rbp)", fp, obj_offset(var) + 8);
    } else {
      char* reg1 = (gp == 0) ? "%al" : "%dl";
      char* reg2 = (gp == 0) ? "%rax" : "%rdx";
      for (int i = 8; i < MIN(16, ty->size); i++) {
        println("  mov %s, %d(%%rbp)", reg1, obj_offset(var) + i);
        println("  shr $8, %s", reg2);
      }
    }
  }
}

static void copy_struct_reg(void) {
  LibcpType* ty = current_fn->ty->return_ty;
  int gp = 0, fp = 0;

  println("  mov %%rax, %%rdi");

  if (has_flonum(ty, 0, 8, 0)) {
    assert(ty->size == 4 || 8 <= ty->size);
    if (ty->size == 4)
      println("  movss (%%rdi), %%xmm0");
    else
      println("  movsd (%%rdi), %%xmm0");
    fp++;
  } else {
    println("  mov $0, %%rax");
    for (int i = MIN(8, ty->size) - 1; i >= 0; i--) {
      println("  shl $8, %%rax");
      println("  mov %d(%%rdi), %%al", i);
    }
    gp++;
  }

  if (ty->size > 8) {
    if (has_flonum(ty, 8, 16, 0)) {
      assert(ty->size == 12 || ty->size == 16);
      if (ty->size == 4)
        println("  movss 8(%%rdi), %%xmm%d", fp);
      else
        println("  movsd 8(%%rdi), %%xmm%d", fp);
    } else {
      char* reg1 = (gp == 0) ? "%al" : "%dl";
      char* reg2 = (gp == 0) ? "%rax" : "%rdx";
      println("  mov $0, %s", reg2);
      for (int i = MIN(16, ty->size) - 1; i >= 8; i--) {
        println("  shl $8, %s", reg2);
        println("  mov %d(%%rdi), %s", i, reg1);
      }
    }
  }
}

static void copy_struct_mem(void) {
  LibcpType* ty = current_fn->ty->return_ty;
  LibcpObj* var = current_fn->params;

  println("  mov %d(%%rbp), %%rdi", obj_offset(var));

  for (int i = 0; i < ty->size; i++) {
    println("  mov %d(%%rax), %%dl", i);
    println("  mov %%dl, %d(%%rdi)", i);
  }
}

static void builtin_alloca(void) {
  // 把大小向上对齐到 16 字节.
  println("  add $15, %%rdi");
  println("  and $0xfffffff0, %%edi");

  // 把临时区域下移 %rdi 字节.
  println("  mov %d(%%rbp), %%rcx", obj_offset(current_fn->alloca_bottom));
  println("  sub %%rsp, %%rcx");
  println("  mov %%rsp, %%rax");
  println("  sub %%rdi, %%rsp");
  println("  mov %%rsp, %%rdx");
  println("1:");
  println("  cmp $0, %%rcx");
  println("  je 2f");
  println("  mov (%%rax), %%r8b");
  println("  mov %%r8b, (%%rdx)");
  println("  inc %%rdx");
  println("  inc %%rax");
  println("  dec %%rcx");
  println("  jmp 1b");
  println("2:");

  // 移动 alloca_bottom 指针.
  println("  mov %d(%%rbp), %%rax", obj_offset(current_fn->alloca_bottom));
  println("  sub %%rdi, %%rax");
  println("  mov %%rax, %d(%%rbp)", obj_offset(current_fn->alloca_bottom));
}

// 为给定节点生成代码.
static void gen_expr(LibcpContext* ctx, LibcpNode* node) {
  // 忠实的 `+`/`-` 仍保留源码形态 (PLAN B2): 元素大小的缩放, `num + ptr`
  // 的规范化以及 libcp_new_arith 所做的转换都在这里完成, 位于发射阶段,
  // 用的是 shaping pass 过去提前调用的同一对 new_add/new_sub.
  // 这里替换节点而不是递归进入: 产物携带原节点的 token, 所以调试行和
  // 求值顺序都与 B2 之前相同. codegen 自己构建的加法节点带标记,
  // 落入下面普通的机器算术发射路径.
  if ((node->kind == ND_ADD || node->kind == ND_SUB) && !cg(node)->is_lowered)
    node = node->kind == ND_ADD ? new_add(ctx, node->lhs, node->rhs, node->tok) :
                                  new_sub(ctx, node->lhs, node->rhs, node->tok);

  println("  .loc %d %d", node->tok->file->file_no, node->tok->line_no);

  switch (node->kind) {
    case ND_NULL_EXPR: return;
    case ND_NUM: {
      switch (node->ty->kind) {
        case TY_FLOAT: {
          union {
            float f32;
            uint32_t u32;
          } u = {node->fval};
          println("  mov $%u, %%eax  # float %Lf", u.u32, node->fval);
          println("  movq %%rax, %%xmm0");
          return;
        }
        case TY_DOUBLE: {
          union {
            double f64;
            uint64_t u64;
          } u = {node->fval};
          println("  mov $%lu, %%rax  # double %Lf", (unsigned long)u.u64, node->fval);
          println("  movq %%rax, %%xmm0");
          return;
        }
        case TY_LDOUBLE: {
          union {
            long double f80;
            uint64_t u64[2];
          } u;
          memset(&u, 0, sizeof(u));
          u.f80 = node->fval;
          println("  mov $%lu, %%rax  # long double %Lf", (unsigned long)u.u64[0], node->fval);
          println("  mov %%rax, -16(%%rsp)");
          println("  mov $%lu, %%rax", (unsigned long)u.u64[1]);
          println("  mov %%rax, -8(%%rsp)");
          println("  fldt -16(%%rsp)");
          return;
        }
      }

      println("  mov $%ld, %%rax", (long)node->val);
      return;
    }
    case ND_SIZEOF:
    case ND_ALIGNOF:
    case ND_TYPES_COMPATIBLE:
    case ND_REG_CLASS:
      // sema 记录在 `val` 中的结论 (PLAN A9.1): 大小, 对齐, 类型兼容性
      // 或寄存器类别 - 全是数值, 类型与它们过去折叠成的字面量相同, 所以
      // 上面的整数形式适用. VLA 的 sizeof 不会到这里: shaping pass 已把
      // 它降级为读取大小变量的逗号表达式.
      println("  mov $%ld, %%rax", (long)node->val);
      return;
    case ND_GENERIC:
      // 选择得到的结论 (PLAN A9.1): 发射被选中的结果表达式.
      gen_expr(ctx, node->generic_sel);
      return;
    case ND_NEG:
      gen_expr(ctx, node->lhs);

      switch (node->ty->kind) {
        case TY_FLOAT:
          println("  mov $1, %%rax");
          println("  shl $31, %%rax");
          println("  movq %%rax, %%xmm1");
          println("  xorps %%xmm1, %%xmm0");
          return;
        case TY_DOUBLE:
          println("  mov $1, %%rax");
          println("  shl $63, %%rax");
          println("  movq %%rax, %%xmm1");
          println("  xorpd %%xmm1, %%xmm0");
          return;
        case TY_LDOUBLE: println("  fchs"); return;
      }

      println("  neg %%rax");
      return;
    case ND_STRING:
      // 字面量的匿名全局对象 (PLAN A9.1: 节点保留自己的 kind): 在表达式
      // 上下文中是数组, 所以下面的 ND_VAR 处理取它的地址, 数组加载是
      // 空操作 - 与降级后的 ND_VAR 发射的指令相同.
    case ND_VAR:
      gen_addr(ctx, node);
      load(node->ty);
      return;
    case ND_MEMBER: {
      gen_addr(ctx, node);
      load(node->ty);

      LibcpMember* mem = node->member;
      if (mem->is_bitfield) {
        println("  shl $%d, %%rax", 64 - mem->bit_width - mem->bit_offset);
        if (mem->ty->is_unsigned)
          println("  shr $%d, %%rax", 64 - mem->bit_width);
        else
          println("  sar $%d, %%rax", 64 - mem->bit_width);
      }
      return;
    }
    case ND_SUBSCRIPT:
      // 值上下文中的 `x[y]` (PLAN B1.1a): 先发射 gen_addr 算出的地址,
      // 再加载指向的对象 - 与旧的 `*(x+y)` 形态发出的指令相同, 其中的和
      // 按需构建.
      gen_addr(ctx, node);
      load(node->ty);
      return;
    case ND_DEREF:
      gen_expr(ctx, node->lhs);
      load(node->ty);
      return;
    case ND_ADDR: gen_addr(ctx, node->lhs); return;
    case ND_ASSIGN:
      gen_addr(ctx, node->lhs);
      push();
      gen_expr(ctx, node->rhs);

      if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
        println("  mov %%rax, %%r8");

        // lhs 是位域时, 需要从内存读取当前值, 再与新值合并.
        LibcpMember* mem = node->lhs->member;
        println("  mov %%rax, %%rdi");

        // `and` 只接受符号扩展的 32 位立即数. 更宽的掩码 (32 位及以上
        // 的位域) 必须先装入寄存器.
        long field_mask = (long)bitfield_mask(mem->bit_width);
        if (field_mask > INT32_MAX || field_mask < INT32_MIN) {
          println("  mov $%ld, %%r9", field_mask);
          println("  and %%r9, %%rdi");
        } else {
          println("  and $%ld, %%rdi", field_mask);
        }

        println("  shl $%d, %%rdi", mem->bit_offset);

        println("  mov (%%rsp), %%rax");
        load(mem->ty);

        long mask = (long)(bitfield_mask(mem->bit_width) << mem->bit_offset);
        println("  mov $%ld, %%r9", ~mask);
        println("  and %%r9, %%rax");
        println("  or %%rdi, %%rax");
        store(node->ty);
        println("  mov %%r8, %%rax");
        return;
      }

      store(node->ty);
      return;
    case ND_STMT_EXPR:
      for (LibcpNode* n = node->body; n; n = n->next)
        gen_stmt(ctx, n);
      return;
    case ND_COMMA:
      gen_expr(ctx, node->lhs);
      gen_expr(ctx, node->rhs);
      return;
    case ND_CAST:
      gen_expr(ctx, node->lhs);
      cast(node->lhs->ty, node->ty);
      return;
    case ND_MEMZERO:
      // `rep stosb` 等价于 `memset(%rdi, %al, %rcx)`.
      println("  mov $%d, %%rcx", node->var->ty->size);
      println("  lea %d(%%rbp), %%rdi", obj_offset(node->var));
      println("  mov $0, %%al");
      println("  rep stosb");
      return;
    case ND_COND: {
      if (node->is_elvis) {
        // GNU 的 `a ?: b` (PLAN B1.1e): 测试的值就是结果, 因此留在测试
        // 求值后所在的寄存器里 - rax, xmm0 或 x87 栈顶 - 不需要临时槽.
        // sema 只记录结论 (两个分支的公共类型), 不转换忠实的操作数, 所以
        // 每个分支在这里转换到该类型. 标签名沿用旧的 `tmp = a, tmp ? tmp : b`
        // 降级所发射的名字.
        int c = count();
        gen_expr(ctx, node->cond);

        // cmp_zero 会消耗它在 x87 栈上比较的那个值; 这里复制一份,
        // 留下的那一份作为真值分支的结果.
        if (node->cond->ty->kind == TY_LDOUBLE)
          println("  fld %%st(0)");

        cmp_zero(node->cond->ty);
        println("  je .L.else.%d", c);
        cast(node->cond->ty, node->ty);
        println("  jmp .L.end.%d", c);
        println(".L.else.%d:", c);

        // 假值分支在计算自己的值之前丢弃测试的值.
        if (node->cond->ty->kind == TY_LDOUBLE)
          println("  fstp %%st(0)");

        gen_expr(ctx, node->els);
        cast(node->els->ty, node->ty);
        println(".L.end.%d:", c);
        return;
      }
      int c = count();
      gen_expr(ctx, node->cond);
      cmp_zero(node->cond->ty);
      println("  je .L.else.%d", c);
      gen_expr(ctx, node->then);
      println("  jmp .L.end.%d", c);
      println(".L.else.%d:", c);
      gen_expr(ctx, node->els);
      println(".L.end.%d:", c);
      return;
    }
    case ND_NOT:
      gen_expr(ctx, node->lhs);
      cmp_zero(node->lhs->ty);
      println("  sete %%al");
      println("  movzx %%al, %%rax");
      return;
    case ND_BITNOT:
      gen_expr(ctx, node->lhs);
      println("  not %%rax");
      return;
    case ND_LOGAND: {
      int c = count();
      gen_expr(ctx, node->lhs);
      cmp_zero(node->lhs->ty);
      println("  je .L.false.%d", c);
      gen_expr(ctx, node->rhs);
      cmp_zero(node->rhs->ty);
      println("  je .L.false.%d", c);
      println("  mov $1, %%rax");
      println("  jmp .L.end.%d", c);
      println(".L.false.%d:", c);
      println("  mov $0, %%rax");
      println(".L.end.%d:", c);
      return;
    }
    case ND_LOGOR: {
      int c = count();
      gen_expr(ctx, node->lhs);
      cmp_zero(node->lhs->ty);
      println("  jne .L.true.%d", c);
      gen_expr(ctx, node->rhs);
      cmp_zero(node->rhs->ty);
      println("  jne .L.true.%d", c);
      println("  mov $0, %%rax");
      println("  jmp .L.end.%d", c);
      println(".L.true.%d:", c);
      println("  mov $1, %%rax");
      println(".L.end.%d:", c);
      return;
    }
    case ND_FUNCALL: {
      if (node->lhs->kind == ND_VAR && !strcmp(node->lhs->var->name, "alloca")) {
        gen_expr(ctx, node->args);
        println("  mov %%rax, %%rdi");
        builtin_alloca();
        return;
      }

      int stack_args = push_args(ctx, node);
      gen_expr(ctx, node->lhs);

      int gp = 0, fp = 0;

      // 返回类型是大的 struct/union 时, 调用方像传第一个实参那样传入一个
      // 缓冲区指针.
      if (cg(node)->ret_buffer && node->ty->size > 16)
        pop(argreg64[gp++]);

      for (LibcpNode* arg = node->args; arg; arg = arg->next) {
        LibcpType* ty = arg->ty;

        switch (ty->kind) {
          case TY_STRUCT:
          case TY_UNION:
            if (ty->size > 16)
              continue;

            bool fp1 = has_flonum1(ty);
            bool fp2 = has_flonum2(ty);

            if (fp + fp1 + fp2 < FP_MAX && gp + !fp1 + !fp2 < GP_MAX) {
              if (fp1)
                popf(fp++);
              else
                pop(argreg64[gp++]);

              if (ty->size > 8) {
                if (fp2)
                  popf(fp++);
                else
                  pop(argreg64[gp++]);
              }
            }
            break;
          case TY_FLOAT:
          case TY_DOUBLE:
            if (fp < FP_MAX)
              popf(fp++);
            break;
          case TY_LDOUBLE: break;
          default:
            if (gp < GP_MAX)
              pop(argreg64[gp++]);
        }
      }

      println("  mov %%rax, %%r10");
      println("  mov $%d, %%rax", fp);
      println("  call *%%r10");
      println("  add $%d, %%rsp", stack_args * 8);

      depth -= stack_args;

      // 返回类型是 short 时, RAX 高 48 位; 是 bool/char 时, 高 56 位,
      // 都可能含垃圾值. 这里清零高位.
      switch (node->ty->kind) {
        case TY_BOOL: println("  movzx %%al, %%eax"); return;
        case TY_CHAR:
          if (node->ty->is_unsigned)
            println("  movzbl %%al, %%eax");
          else
            println("  movsbl %%al, %%eax");
          return;
        case TY_SHORT:
          if (node->ty->is_unsigned)
            println("  movzwl %%ax, %%eax");
          else
            println("  movswl %%ax, %%eax");
          return;
      }

      // 若返回类型是小 struct, 返回值通过最多两个寄存器传递.
      if (cg(node)->ret_buffer && node->ty->size <= 16) {
        copy_ret_buffer(cg(node)->ret_buffer);
        println("  lea %d(%%rbp), %%rax", obj_offset(cg(node)->ret_buffer));
      }

      return;
    }
    case ND_LABEL_VAL: println("  lea %s(%%rip), %%rax", cg(node)->unique_label); return;
    case ND_CAS: {
      gen_expr(ctx, node->cas_addr);
      push();
      gen_expr(ctx, node->cas_new);
      push();
      gen_expr(ctx, node->cas_old);
      println("  mov %%rax, %%r8");
      load(node->cas_old->ty->base);
      pop("%rdx");  // 新值
      pop("%rdi");  // 地址

      int sz = node->cas_addr->ty->base->size;
      println("  lock cmpxchg %s, (%%rdi)", reg_dx(ctx, sz));
      println("  sete %%cl");
      println("  je 1f");
      println("  mov %s, (%%r8)", reg_ax(ctx, sz));
      println("1:");
      println("  movzbl %%cl, %%eax");
      return;
    }
    case ND_EXCH: {
      gen_expr(ctx, node->lhs);
      push();
      gen_expr(ctx, node->rhs);
      pop("%rdi");

      int sz = node->lhs->ty->base->size;
      println("  xchg %s, (%%rdi)", reg_ax(ctx, sz));
      return;
    }
  }

  // 发射器认识的比较形状是 `<` 和 `<=`; 忠实的 `>` / `>=` 现在保留自己的
  // 操作数 (PLAN B1.1c), 所以原先由 shaping pass 写进树里的交换改在这里做,
  // 落在一个专为本次发射构造, 随后丢弃的节点上. 操作数是同一批对象, 角色
  // 也仍是那次交换赋予的角色, 因此求值顺序与发射出的条件和 B 之前的树完全
  // 一致 - 整数路径读取的符号性来自交换后位于左边的那个操作数, 求值器读取
  // 忠实的一对操作数时也是如此.
  if (node->kind == ND_GT || node->kind == ND_GE) {
    LibcpNode* swapped =
        libcp_new_binary(ctx, node->kind == ND_GT ? ND_LT : ND_LE, node->rhs, node->lhs, node->tok);
    swapped->ty = node->ty;
    gen_expr(ctx, swapped);
    return;
  }

  switch (node->lhs->ty->kind) {
    case TY_FLOAT:
    case TY_DOUBLE: {
      gen_expr(ctx, node->rhs);
      pushf();
      gen_expr(ctx, node->lhs);
      popf(1);

      char* sz = (node->lhs->ty->kind == TY_FLOAT) ? "ss" : "sd";

      switch (node->kind) {
        case ND_ADD: println("  add%s %%xmm1, %%xmm0", sz); return;
        case ND_SUB: println("  sub%s %%xmm1, %%xmm0", sz); return;
        case ND_MUL: println("  mul%s %%xmm1, %%xmm0", sz); return;
        case ND_DIV: println("  div%s %%xmm1, %%xmm0", sz); return;
        case ND_EQ:
        case ND_NE:
        case ND_LT:
        case ND_LE:
          println("  ucomi%s %%xmm0, %%xmm1", sz);

          if (node->kind == ND_EQ) {
            println("  sete %%al");
            println("  setnp %%dl");
            println("  and %%dl, %%al");
          } else if (node->kind == ND_NE) {
            println("  setne %%al");
            println("  setp %%dl");
            println("  or %%dl, %%al");
          } else if (node->kind == ND_LT) {
            println("  seta %%al");
          } else {
            println("  setae %%al");
          }

          println("  and $1, %%al");
          println("  movzb %%al, %%rax");
          return;
      }

      libcp_error_tok(ctx, node->tok, "invalid expression");
    }
    case TY_LDOUBLE: {
      gen_expr(ctx, node->lhs);
      gen_expr(ctx, node->rhs);

      switch (node->kind) {
        case ND_ADD: println("  faddp"); return;
        case ND_SUB: println("  fsubrp"); return;
        case ND_MUL: println("  fmulp"); return;
        case ND_DIV: println("  fdivrp"); return;
        case ND_EQ:
        case ND_NE:
        case ND_LT:
        case ND_LE:
          println("  fcomip");
          println("  fstp %%st(0)");

          if (node->kind == ND_EQ)
            println("  sete %%al");
          else if (node->kind == ND_NE)
            println("  setne %%al");
          else if (node->kind == ND_LT)
            println("  seta %%al");
          else
            println("  setae %%al");

          println("  movzb %%al, %%rax");
          return;
      }

      libcp_error_tok(ctx, node->tok, "invalid expression");
    }
  }

  gen_expr(ctx, node->rhs);
  push();
  gen_expr(ctx, node->lhs);
  pop("%rdi");

  char *ax, *di, *dx;

  if (node->lhs->ty->kind == TY_LONG || node->lhs->ty->base) {
    ax = "%rax";
    di = "%rdi";
    dx = "%rdx";
  } else {
    ax = "%eax";
    di = "%edi";
    dx = "%edx";
  }

  switch (node->kind) {
    case ND_ADD: println("  add %s, %s", di, ax); return;
    case ND_SUB: println("  sub %s, %s", di, ax); return;
    case ND_MUL: println("  imul %s, %s", di, ax); return;
    case ND_DIV:
    case ND_MOD:
      if (node->ty->is_unsigned) {
        println("  mov $0, %s", dx);
        println("  div %s", di);
      } else {
        if (node->lhs->ty->size == 8)
          println("  cqo");
        else
          println("  cdq");
        println("  idiv %s", di);
      }

      if (node->kind == ND_MOD)
        println("  mov %%rdx, %%rax");
      return;
    case ND_BITAND: println("  and %s, %s", di, ax); return;
    case ND_BITOR: println("  or %s, %s", di, ax); return;
    case ND_BITXOR: println("  xor %s, %s", di, ax); return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
      println("  cmp %s, %s", di, ax);

      if (node->kind == ND_EQ) {
        println("  sete %%al");
      } else if (node->kind == ND_NE) {
        println("  setne %%al");
      } else if (node->kind == ND_LT) {
        if (node->lhs->ty->is_unsigned)
          println("  setb %%al");
        else
          println("  setl %%al");
      } else if (node->kind == ND_LE) {
        if (node->lhs->ty->is_unsigned)
          println("  setbe %%al");
        else
          println("  setle %%al");
      }

      println("  movzb %%al, %%rax");
      return;
    case ND_SHL:
      println("  mov %%rdi, %%rcx");
      println("  shl %%cl, %s", ax);
      return;
    case ND_SHR:
      println("  mov %%rdi, %%rcx");
      if (node->lhs->ty->is_unsigned)
        println("  shr %%cl, %s", ax);
      else
        println("  sar %%cl, %s", ax);
      return;
  }

  libcp_error_tok(ctx, node->tok, "invalid expression");
}

static void gen_stmt(LibcpContext* ctx, LibcpNode* node) {
  println("  .loc %d %d", node->tok->file->file_no, node->tok->line_no);

  switch (node->kind) {
    case ND_IF: {
      int c = count();
      gen_expr(ctx, node->cond);
      cmp_zero(node->cond->ty);
      println("  je  .L.else.%d", c);
      gen_stmt(ctx, node->then);
      println("  jmp .L.end.%d", c);
      println(".L.else.%d:", c);
      if (node->els)
        gen_stmt(ctx, node->els);
      println(".L.end.%d:", c);
      return;
    }
    case ND_WHILE: {
      // 忠实的循环 (PLAN B1.2a): 发射结果与旧的 `while` 到 `for` 改写
      // 所产生的一致 - 没有 init, 没有 increment, 所以 continue 标签
      // 落在收尾的跳转上 - 循环体内生效的是本循环的标签.
      int c          = count();
      char* brk      = cur_brk_label;
      char* cont     = cur_cont_label;
      cur_brk_label  = cg(node)->brk_label;
      cur_cont_label = cg(node)->cont_label;

      println(".L.begin.%d:", c);
      gen_expr(ctx, node->cond);
      cmp_zero(node->cond->ty);
      println("  je %s", cg(node)->brk_label);
      gen_stmt(ctx, node->then);
      println("%s:", cg(node)->cont_label);
      println("  jmp .L.begin.%d", c);
      println("%s:", cg(node)->brk_label);

      cur_brk_label  = brk;
      cur_cont_label = cont;
      return;
    }
    case ND_FOR: {
      int c          = count();
      char* brk      = cur_brk_label;
      char* cont     = cur_cont_label;
      cur_brk_label  = cg(node)->brk_label;
      cur_cont_label = cg(node)->cont_label;

      if (node->init)
        gen_stmt(ctx, node->init);
      println(".L.begin.%d:", c);
      if (node->cond) {
        gen_expr(ctx, node->cond);
        cmp_zero(node->cond->ty);
        println("  je %s", cg(node)->brk_label);
      }
      gen_stmt(ctx, node->then);
      println("%s:", cg(node)->cont_label);
      if (node->inc)
        gen_expr(ctx, node->inc);
      println("  jmp .L.begin.%d", c);
      println("%s:", cg(node)->brk_label);

      cur_brk_label  = brk;
      cur_cont_label = cont;
      return;
    }
    case ND_DO: {
      int c          = count();
      char* brk      = cur_brk_label;
      char* cont     = cur_cont_label;
      cur_brk_label  = cg(node)->brk_label;
      cur_cont_label = cg(node)->cont_label;

      println(".L.begin.%d:", c);
      gen_stmt(ctx, node->then);
      println("%s:", cg(node)->cont_label);
      gen_expr(ctx, node->cond);
      cmp_zero(node->cond->ty);
      println("  jne .L.begin.%d", c);
      println("%s:", cg(node)->brk_label);

      cur_brk_label  = brk;
      cur_cont_label = cont;
      return;
    }
    case ND_SWITCH: {
      // switch 拥有 break 目标但不拥有 continue 目标 (PLAN B1.2a):
      // 外层循环的 continue 标签在循环体内仍然生效.
      char* brk     = cur_brk_label;
      cur_brk_label = cg(node)->brk_label;

      gen_expr(ctx, node->cond);

      for (LibcpNode* n = cg(node)->case_next; n; n = cg(n)->case_next) {
        bool wide = node->cond->ty->size == 8;
        char* ax  = wide ? "%rax" : "%eax";
        char* di  = wide ? "%rdi" : "%edi";

        // 32 位 switch 的标签按控制表达式的宽度收窄为 int; 64 位 switch
        // 比较完整取值. `cmp` 只接受符号扩展的 imm32, 因此放不下的 64 位
        // 标签先装入 %rdi (范围上界用 %rsi).
        long begin    = wide ? n->begin : (long)(int)n->begin;
        long span     = wide ? n->end - n->begin : (long)(int)n->end - (long)(int)n->begin;
        bool wide_imm = wide && (begin < INT32_MIN || begin > INT32_MAX || span < INT32_MIN ||
                                 span > INT32_MAX);

        if (n->begin == n->end) {
          if (begin < INT32_MIN || begin > INT32_MAX) {
            println("  mov $%ld, %s", begin, di);
            println("  cmp %s, %s", di, ax);
          } else {
            println("  cmp $%ld, %s", begin, ax);
          }
          println("  je %s", cg(n)->unique_label);
          continue;
        }

        // [GNU] case 范围
        println("  mov %s, %s", ax, di);
        if (wide_imm) {
          println("  mov $%ld, %%rsi", begin);
          println("  sub %%rsi, %s", di);
          println("  mov $%ld, %%rsi", span);
          println("  cmp %%rsi, %s", di);
        } else {
          println("  sub $%ld, %s", begin, di);
          println("  cmp $%ld, %s", span, di);
        }
        println("  jbe %s", cg(n)->unique_label);
      }

      if (cg(node)->default_case)
        println("  jmp %s", cg(cg(node)->default_case)->unique_label);

      println("  jmp %s", cg(node)->brk_label);
      gen_stmt(ctx, node->then);
      println("%s:", cg(node)->brk_label);

      cur_brk_label = brk;
      return;
    }
    case ND_CASE:
      println("%s:", cg(node)->unique_label);
      gen_stmt(ctx, node->lhs);
      return;
    case ND_BLOCK:
      for (LibcpNode* n = node->body; n; n = n->next)
        gen_stmt(ctx, n);
      return;
    case ND_DECL: {
      // 到达发射器的声明记录 (PLAN B1.2b): 块作用域 static (sema 的
      // 序列化器已写好其数据镜像) 与没有初始化器的固定大小对象不产生代码;
      // 带初始化器的对象变成逗号链, 这条链过去由 pass 里的展开构造 (先清零,
      // 再逐元素赋值), 现在在这里发射.
      if (node->attr.is_static || !node->init_resolved)
        return;

      LibcpNode* chain = lvar_init_comma(ctx, node->var, node->init_resolved, node->decl_init->tok);
      node->decl_init  = NULL;
      node->init_resolved = NULL;
      libcp_add_type(ctx, chain);
      gen_expr(ctx, chain);
      return;
    }
    case ND_BREAK:
      // 发射时按最内层的循环或 switch 解析 (PLAN B1.2a); sema 的检查
      // 已经拒绝了游离的 break.
      println("  jmp %s", cur_brk_label);
      return;
    case ND_CONTINUE:
      // 最内层的循环, 无论中间隔着多少层 switch.
      println("  jmp %s", cur_cont_label);
      return;
    case ND_GOTO: println("  jmp %s", cg(node)->unique_label); return;
    case ND_GOTO_EXPR:
      gen_expr(ctx, node->lhs);
      println("  jmp *%%rax");
      return;
    case ND_LABEL:
      println("%s:", cg(node)->unique_label);
      gen_stmt(ctx, node->lhs);
      return;
    case ND_RETURN:
      if (node->lhs) {
        gen_expr(ctx, node->lhs);
        LibcpType* ty = node->lhs->ty;

        switch (ty->kind) {
          case TY_STRUCT:
          case TY_UNION:
            if (ty->size <= 16)
              copy_struct_reg();
            else
              copy_struct_mem();
            break;
        }
      }

      println("  jmp .L.return.%s", current_fn->name);
      return;
    case ND_EXPR_STMT: gen_expr(ctx, node->lhs); return;
    case ND_ASM: println("  %s", node->asm_str); return;
  }

  libcp_error_tok(ctx, node->tok, "invalid statement");
}

// shaping pass 的最终形态: 它是 sema 在标注后下降中做过的控制流那一半
// 的等价物, 再加上旧实现在 pass 内做过的那整套表达式与声明改写 (下标,
// 成员, 复合赋值与 inc/dec, 加法缩放与比较, 调用, 字符串与其他结论,
// elvis, VLA 尺寸, 声明与初始化器 - PLAN 步骤 A2.1 至 A9.2); 其中一部分
// 改写现在已移到发射时完成. 留在 pass 里的是: 为循环分配 break/continue
// 标签, 为 switch 分配 break 标签, 把 case 标签链接进所属的 switch, 给每个
// 标签分配唯一名, 把 goto 与 [GNU] label-value 引用解析到这些标签, 为
// `while`/`do`/`for` 保留节点形式 (gen_stmt 直接发射, break/continue
// 在发射时解析到最内层目标), 展开带 VLA 的声明记录, 并把复合赋值与
// inc/dec, VLA sizeof, 复合字面量等改写成发射器读取的形式. 这些下降里
// 原先附带的检查 - 游离诊断, 未声明标签的配对和操作数检查 - 留在 sema,
// 所以这个 pass 只处理通过检查的树. 它遵循的六条不变式写在文件开头.
//
// 标签名保持 `.L..%d` 格式, 沿用 sema 匿名对象所用的同一个编号空间, 从
// 库计数器停下的地方继续 (入口处调用 libcp_unique_name_next): 快照
// 归一化器只重编号这种后缀形状, 所以换一个前缀即使只是纯重编号也会表现为
// diff, 而从零开始的新序列会与 sema 已经分配的对象名冲突.
static int label_base;  // 计数器停下处的取值, 只在入口取一次
static int label_seq;   // 此后发出的标签数

static LibcpNode* current_switch;
static LibcpNodeArray cg_labels;  // 当前正在 shape 的函数的标签

static char* new_label(LibcpContext* ctx) {
  return libcp_format(ctx, ".L..%d", label_base + label_seq++);
}

static void shape_node(LibcpContext* ctx, LibcpNode* node);
static bool expand_decl(LibcpContext* ctx, LibcpNode* node);
static void shape_init_exprs(LibcpContext* ctx, LibcpInitTree* init);

// 声明的类型是否在任何一层涉及 VLA: 它的运行时大小槽必须在
// assign_lvar_offsets 运行前存在, 所以这类记录仍在 pass 内展开. 其它声明
// 保持原位到达发射器, 由发射器在发射时构造初始化器链 (PLAN B1.2b).
static bool decl_needs_vla_size(LibcpNode* node) {
  for (LibcpType* t = node->var->ty; t; t = t->base)
    if (t->kind == TY_VLA)
      return true;
  return false;
}

// 遍历语句链并原地编辑: 带 VLA 的声明记录展开成它的语句 (PLAN A8.1),
// 留下来的声明保持原位交给发射器 (PLAN B1.2b), 其它记录 - typedef,
// 枚举常量, extern 声明与 [GNU] 嵌套函数定义 - 完全不产生语句, 在这里
// 被摘链 (PLAN A8.2). 嵌套函数体不从这里进入: 程序列表才是它的归属,
// 这个 pass 从那里恰好遍历它一次 (contract 3(e)).
static void shape_chain(LibcpContext* ctx, LibcpNode** head) {
  for (LibcpNode** pp = head; *pp;) {
    LibcpNode* n = *pp;

    switch (n->kind) {
      case ND_TYPEDEF:
      case ND_ENUM_CONST:
      case ND_GVAR_DECL:
      case ND_FUNCDEF: *pp = n->next; continue;
      case ND_DECL:
        if (decl_needs_vla_size(n))
          expand_decl(ctx, n);
        else
          // 记录保留; 已解析初始化器可能持有的那些残留节点种类靠这次
          // 遍历到达 (op=, 一次 inc/dec, 一个 VLA sizeof, 一个复合字面量,
          // 一个返回 struct 的调用 - 加法缩放已不在其中, 它在发射时
          // 降级, PLAN B2).
          shape_init_exprs(ctx, n->init_resolved);
        pp = &(*pp)->next;
        continue;
      default:
        shape_node(ctx, n);
        pp = &(*pp)->next;
        continue;
    }
  }
}

// 下降节点拥有的每棵子树, 语句与表达式一视同仁: break 可以出现在
// 语句表达式里, 而语句表达式是一个持有语句链的表达式节点.
static void shape_children(LibcpContext* ctx, LibcpNode* node) {
  shape_node(ctx, node->lhs);
  shape_node(ctx, node->rhs);
  shape_node(ctx, node->cond);
  shape_node(ctx, node->then);
  shape_node(ctx, node->els);
  shape_chain(ctx, &node->init);
  shape_node(ctx, node->inc);
  shape_chain(ctx, &node->body);
  shape_chain(ctx, &node->args);
  shape_node(ctx, node->cas_addr);
  shape_node(ctx, node->cas_old);
  shape_node(ctx, node->cas_new);
}

// 复合赋值改写引入的临时变量是消费者的槽位: 在这里创建, 以便在
// assign_lvar_offsets 运行前进入栈帧布局 (PLAN contract 3).
static LibcpObj* new_lvar(char* name, LibcpType* ty) {
  LibcpObj* var      = calloc(1, sizeof(LibcpObj));
  var->name          = name;
  var->ty            = ty;
  var->align         = ty->align;
  var->is_local      = true;
  var->next          = current_fn->locals;
  current_fn->locals = var;
  return var;
}

// 指针上加法运算符的被缩放操作数: `p + n` 前进 sizeof(*p) * n 而不是 n;
// 被指向类型是 VLA 时, 按它的运行时大小而不是编译期大小缩放.
static LibcpNode* scale_rhs(LibcpContext* ctx, LibcpType* ptr_ty, LibcpNode* rhs, LibcpToken* tok) {
  if (ptr_ty->base->kind == TY_VLA)
    return libcp_new_binary(ctx,
                            ND_MUL,
                            rhs,
                            libcp_new_var_node(ctx, cg_type(ptr_ty->base)->vla_size, tok),
                            tok);
  return libcp_new_binary(ctx, ND_MUL, rhs, libcp_new_long(ctx, ptr_ty->base->size, tok), tok);
}

// 把加法节点标记为由下面的 lowering helper 构造. 它的操作数已经带着
// 将要相加的字节偏移 (或 `ptr - ptr` 的除法要缩小成元素个数的字节差),
// 所以发射器必须把它当作普通机器算术发射 - gen_expr 会自行降级忠实的
// `+`/`-`, 若不标记就会再缩放一次 (PLAN B2).
static LibcpNode* mark_lowered(LibcpNode* node) {
  cg(node)->is_lowered = true;
  return node;
}

// 在 C 中, `+` 运算符被重载来执行指针算术. 若 p 是指针, p+n 加到 p 的值
// 上的不是 n 而是 sizeof(*p)*n, 使 p+n 指向 p 之后 n 个元素 (而不是 n 个
// 字节) 的位置. 换言之, 在把整数值加到指针值之前需要先对它做缩放. 这个
// 函数负责缩放.
//
// 它既是 codegen 自身 builder 调用的 helper, 也是发射器在发射时执行的
// 忠实降级的定义 (PLAN B2); 无论哪条路径, 产物都带标记.
static LibcpNode* new_add(LibcpContext* ctx, LibcpNode* lhs, LibcpNode* rhs, LibcpToken* tok) {
  libcp_add_type(ctx, lhs);
  libcp_add_type(ctx, rhs);

  // num + num
  if (libcp_is_numeric(lhs->ty) && libcp_is_numeric(rhs->ty))
    return mark_lowered(libcp_new_arith(ctx, ND_ADD, lhs, rhs, tok));

  if (lhs->ty->base && rhs->ty->base)
    libcp_error_tok(ctx, tok, "invalid operands");

  // 把 `num + ptr` 规范化为 `ptr + num`.
  if (!lhs->ty->base && rhs->ty->base) {
    LibcpNode* tmp = lhs;
    lhs            = rhs;
    rhs            = tmp;
  }

  // ptr + num; 指向 VLA 的指针按其运行时大小缩放
  return mark_lowered(libcp_new_arith(ctx, ND_ADD, lhs, scale_rhs(ctx, lhs->ty, rhs, tok), tok));
}

// 与 `+` 一样, `-` 对指针类型同样重载.
static LibcpNode* new_sub(LibcpContext* ctx, LibcpNode* lhs, LibcpNode* rhs, LibcpToken* tok) {
  libcp_add_type(ctx, lhs);
  libcp_add_type(ctx, rhs);

  // num - num
  if (libcp_is_numeric(lhs->ty) && libcp_is_numeric(rhs->ty))
    return mark_lowered(libcp_new_arith(ctx, ND_SUB, lhs, rhs, tok));

  // 指向 VLA 的指针 - num
  if (lhs->ty->base && lhs->ty->base->kind == TY_VLA)
    return mark_lowered(libcp_new_arith(ctx, ND_SUB, lhs, scale_rhs(ctx, lhs->ty, rhs, tok), tok));

  // ptr - num
  if (lhs->ty->base && libcp_is_integer(rhs->ty))
    return mark_lowered(libcp_new_arith(ctx, ND_SUB, lhs, scale_rhs(ctx, lhs->ty, rhs, tok), tok));

  // ptr - ptr: 返回两者之间相隔多少个元素. 差值本身是元素个数,
  // 因此类型为 long 且不再转换; 只有其后的除法被转换. 减法节点
  // 也带标记 - 它是原始字节差, 会像构建出的 `ptr + num` 一样在
  // `DIV` 之下经过发射器.
  if (lhs->ty->base && rhs->ty->base) {
    LibcpNode* node = mark_lowered(libcp_new_binary(ctx, ND_SUB, lhs, rhs, tok));
    node->ty        = libcp_ty_long;
    return libcp_new_arith(ctx, ND_DIV, node, libcp_new_num(ctx, lhs->ty->base->size, tok), tok);
  }

  libcp_error_tok(ctx, tok, "invalid operands");
}

// 为计算 VLA 大小生成代码: 类型持有的每个 VLA 一条赋值, 最内层
// 在前依次串联; 一个都不持有时返回 NULL - 所以固定大小对象的
// 声明完全不产生代码. A7.1 从 sema 移来 (有一个步骤里它曾是本
// 函数的镜像): 大小变量是这个消费者的槽位, 这也正是构建器不能
// 再留在库里的原因.
static LibcpNode* compute_vla_size(LibcpContext* ctx, LibcpType* ty, LibcpToken* tok) {
  LibcpNode* node = ty->base ? compute_vla_size(ctx, ty->base, tok) : NULL;

  if (ty->kind != TY_VLA)
    return node;

  LibcpNode* base_sz;
  if (ty->base->kind == TY_VLA)
    base_sz = libcp_new_var_node(ctx, cg_type(ty->base)->vla_size, tok);
  else
    base_sz = libcp_new_num(ctx, ty->base->size, tok);

  cg_type(ty)->vla_size = new_lvar("", libcp_ty_ulong);
  LibcpNode* expr       = libcp_new_binary(ctx,
                                     ND_ASSIGN,
                                     libcp_new_var_node(ctx, cg_type(ty)->vla_size, tok),
                                     libcp_new_binary(ctx, ND_MUL, ty->vla_len, base_sz, tok),
                                     tok);
  return node ? libcp_new_binary(ctx, ND_COMMA, node, expr, tok) : expr;
}

// VLA 类型的 `sizeof`: 对其运行时大小变量的引用; 若该类型尚未
// 计算过大小 (例如对全新类型写 `sizeof(int[n])`), 先计算.
static LibcpNode* vla_size_expr(LibcpContext* ctx, LibcpType* ty, LibcpToken* tok) {
  if (cg_type(ty)->vla_size)
    return libcp_new_var_node(ctx, cg_type(ty)->vla_size, tok);
  LibcpNode* lhs = compute_vla_size(ctx, ty, tok);
  return libcp_new_binary(ctx,
                          ND_COMMA,
                          lhs,
                          libcp_new_var_node(ctx, cg_type(ty)->vla_size, tok),
                          tok);
}

// 一条初始化赋值的赋值目标表达式 (PLAN A8.1, 从 sema 移来):
// 从变量到该记录所初始化元素的路径. 下标步骤直接构建为缩放加法
// 之上的解引用 - 忠实下标会降级成的形状 - 因此中间节点不需要
// 再走一遍 (contract 3(d)).
static LibcpNode* init_desg_expr(LibcpContext* ctx, LibcpInitPath* desg, LibcpToken* tok) {
  if (desg->var)
    return libcp_new_var_node(ctx, desg->var, tok);

  if (desg->member) {
    LibcpNode* node = libcp_new_unary(ctx, ND_MEMBER, init_desg_expr(ctx, desg->next, tok), tok);
    node->member    = desg->member;
    return node;
  }

  LibcpNode* lhs  = init_desg_expr(ctx, desg->next, tok);
  LibcpNode* add  = new_add(ctx, lhs, libcp_new_num(ctx, desg->idx, tok), tok);
  LibcpNode* node = libcp_new_unary(ctx, ND_DEREF, add, tok);
  node->ty        = add->ty->base;
  return node;
}

// 构建局部初始化器的赋值链, 遍历已解析的树 (A8.1 从 sema 移来).
// 聚合类型递归进入子节点; union 只走记录选中的成员; 部分列表
// 由下面 lvar_init_comma 的 MEMZERO 打头.
static LibcpNode* create_lvar_init(LibcpContext* ctx,
                                   LibcpInitTree* init,
                                   LibcpType* ty,
                                   LibcpInitPath* desg,
                                   LibcpToken* tok) {
  if (ty->kind == TY_ARRAY) {
    LibcpNode* node = libcp_new_node(ctx, ND_NULL_EXPR, tok);
    for (int i = 0; i < ty->array_len; i++) {
      LibcpInitPath desg2 = {desg, i};
      LibcpNode* rhs      = create_lvar_init(ctx, init->children[i], ty->base, &desg2, tok);
      node                = libcp_new_binary(ctx, ND_COMMA, node, rhs, tok);
    }
    return node;
  }

  if (ty->kind == TY_STRUCT && !init->expr) {
    LibcpNode* node = libcp_new_node(ctx, ND_NULL_EXPR, tok);

    for (LibcpMember* mem = ty->members; mem; mem = mem->next) {
      LibcpInitPath desg2 = {desg, 0, mem};
      LibcpNode* rhs      = create_lvar_init(ctx, init->children[mem->idx], mem->ty, &desg2, tok);
      node                = libcp_new_binary(ctx, ND_COMMA, node, rhs, tok);
    }
    return node;
  }

  if (ty->kind == TY_UNION) {
    LibcpMember* mem    = init->mem ? init->mem : ty->members;
    LibcpInitPath desg2 = {desg, 0, mem};
    return create_lvar_init(ctx, init->children[mem->idx], mem->ty, &desg2, tok);
  }

  if (!init->expr)
    return libcp_new_node(ctx, ND_NULL_EXPR, tok);

  LibcpNode* lhs = init_desg_expr(ctx, desg, tok);
  return libcp_new_binary(ctx, ND_ASSIGN, lhs, init->expr, tok);
}

// 对已解析初始化记录持有的每个表达式各整形一次. 展开构建的指定
// 初始化器目标已是降级形态 - 再对它们跑一遍 pass 会对本 pass 刚
// 构建的节点重复执行表达式分支 (A4.1 的教训) - 但初始化器表达式
// 是旧节点, 树中没有别处引用它们; 被多条记录共享的表达式 (范围
// 指定初始化器) 会随每条记录各到达一次, 同一节点因此被多次整形.
static void shape_init_exprs(LibcpContext* ctx, LibcpInitTree* init);

// 构建由已解析初始化树初始化局部变量的 MEMZERO + 赋值逗号链.
// `tok` 为合成节点提供锚点; 记录的首个 token 就是解析器过去给
// 这些节点作锚点的位置. A8.1 随声明展开从 sema 移来; sema 自身
// 不再构建它.
static LibcpNode*
    lvar_init_comma(LibcpContext* ctx, LibcpObj* var, LibcpInitTree* init, LibcpToken* tok) {
  LibcpInitPath desg = {NULL, 0, NULL, var};

  // 给定部分初始化器列表时, 标准要求未指定的元素置 0. 这里直接
  // 把变量的整块内存清零, 再用用户提供的值初始化.
  LibcpNode* lhs = libcp_new_node(ctx, ND_MEMZERO, tok);
  lhs->var       = var;

  LibcpNode* rhs = create_lvar_init(ctx, init, var->ty, &desg, tok);
  return libcp_new_binary(ctx, ND_COMMA, lhs, rhs, tok);
}

static void shape_init_exprs(LibcpContext* ctx, LibcpInitTree* init) {
  if (!init)
    return;

  if (init->expr) {
    shape_node(ctx, init->expr);
    return;
  }

  if (!init->children)
    return;

  if (init->ty->kind == TY_ARRAY) {
    for (int i = 0; i < init->ty->array_len; i++)
      shape_init_exprs(ctx, init->children[i]);
    return;
  }

  for (LibcpMember* mem = init->ty->members; mem; mem = mem->next)
    shape_init_exprs(ctx, init->children[mem->idx]);
}

// 把类型涉及 VLA 的声明记录展开成它所代表的语句, 并说明它原先
// 所在的链位置是否保留一条语句. 不涉及 VLA 的声明留在链里, 由
// 发射器在发射时展开 (PLAN B1.2b) - 只有运行时大小槽必须在分配
// 帧布局之前存在, 而这正是本函数这一半产生的东西. 记录到达时与
// 解析器写下的完全一致: sema 已跑过检查, 初始化记录保持已解析,
// 块作用域 static 的数据镜像已在 .data 中. A8.1 从 sema 移来;
// VLA 这一半在 A7.1 随大小槽一起移来.
static bool expand_decl(LibcpContext* ctx, LibcpNode* node) {
  LibcpObj* var   = node->var;
  LibcpToken* tok = node->tok;

  // 块作用域 static 不产生语句: 其数据镜像已由标注 pass 序列化.
  if (node->attr.is_static)
    return false;

  LibcpNode* vla_size = NULL;
  for (LibcpType* t = var->ty; t; t = t->base) {
    if (t->kind == TY_VLA) {
      // 大小计算嵌入的维度表达式从未经过本 pass - 类型是它们唯一
      // 的引用. 先整形它们, 最内层在前, 即计算遍历的顺序; 已经
      // 算过大小的类型在同类更早的声明里到过这里 (两个对象共享
      // 的 typedef), 其维度当时已整形.
      if (!cg_type(var->ty)->vla_size)
        shape_node(ctx, t->vla_len);
      vla_size = compute_vla_size(ctx, var->ty, tok);
      break;
    }
  }

  LibcpNode* lowered = NULL;

  if (var->ty->kind == TY_VLA) {
    // `var = alloca(<size>)`: 大小来自上面刚计算的大小链.
    lowered = libcp_new_binary(
        ctx,
        ND_ASSIGN,
        libcp_new_vla_ptr(ctx, var, tok),
        libcp_new_alloca(ctx, libcp_new_var_node(ctx, cg_type(var->ty)->vla_size, tok)),
        tok);
  } else if (node->init_resolved) {
    // 初始化器逗号链; 指向 VLA 的指针也走到这里, 大小链排在
    // 赋值之前.
    lowered         = lvar_init_comma(ctx, var, node->init_resolved, node->decl_init->tok);
    node->decl_init = NULL;
    // 初始化器表达式从未经过本 pass, 树中也没有别处引用它们;
    // 大小链与指定初始化器目标已是降级形态, 不再触碰.
    shape_init_exprs(ctx, node->init_resolved);
    node->init_resolved = NULL;
  }

  // 没有初始化器的固定大小对象不产生语句.
  if (!vla_size && !lowered)
    return false;

  node->kind = ND_EXPR_STMT;
  node->lhs  = (vla_size && lowered) ? libcp_new_binary(ctx, ND_COMMA, vla_size, lowered, tok) :
                                       (lowered ? lowered : vla_size);
  libcp_add_type(ctx, node);
  return true;
}

// 在操作数的指针缩放已应用的前提下, 构建复合赋值或原子重试循环
// 所需的 `lhs op rhs` 组合. 加法运算符走 libcp_new_arith, 即
// `+`/`-` 产生的形状; 其余交给 libcp_add_type, 它对乘除与位运算
// 做转换, 移位保持不变.
static LibcpNode*
    combine(LibcpContext* ctx, LibcpNodeKind op, LibcpNode* lhs, LibcpNode* rhs, LibcpToken* tok) {
  // 加法情形收到的操作数已经过 new_add/new_sub (这里把读-改-写
  // 的值与它缩放后的操作数组合), 所以也只是普通的机器算术节点.
  if (op == ND_ADD || op == ND_SUB)
    return mark_lowered(libcp_new_arith(ctx, op, lhs, rhs, tok));

  LibcpNode* node = libcp_new_binary(ctx, op, lhs, rhs, tok);
  libcp_add_type(ctx, node);
  return node;
}

// 为由 node->op 驱动的复合赋值构建 `lhs op rhs` 操作数表达式.
static LibcpNode* compound_op(LibcpContext* ctx, LibcpNode* node, LibcpNode* lhs, LibcpToken* tok) {
  LibcpNode* rhs = node->rhs;

  // `+=`/`-=` 走 new_add/new_sub, 好让操作数检查运行, 指针算术也
  // 像普通 `+`/`-` 那样精确缩放; 它们最终选定的操作数就是重建
  // 表达式与 `lhs` 组合所用的那个.
  if (node->op == ND_ADD || node->op == ND_SUB) {
    LibcpNode* scaled = node->op == ND_ADD ? new_add(ctx, node->lhs, node->rhs, tok) :
                                             new_sub(ctx, node->lhs, node->rhs, tok);
    rhs               = scaled->rhs;
  }

  return combine(ctx, node->op, lhs, rhs, tok);
}

// 把 op= 运算符转换成包含赋值的表达式.
//
// `node` 是 ND_ASSIGN, 其 `op` 保存复合赋值运算符. 一般情况下,
// `A op= C` 转换成 `tmp = &A, *tmp = *tmp op C`.
// 但若给定表达式形如 `A.x op= C`, 输入转换成
// `tmp = &A, (*tmp).x = (*tmp).x op C`, 以处理对位域的赋值.
static LibcpNode* to_assign(LibcpContext* ctx, LibcpNode* node) {
  libcp_add_type(ctx, node->lhs);
  libcp_add_type(ctx, node->rhs);
  LibcpToken* tok = node->tok;

  // 把 `A.x op= C` 转换成 `tmp = &A, (*tmp).x = (*tmp).x op C`.
  // 成员现在保留其标记 (PLAN B1.1b), 所以成员所属对象的地址从
  // 标记读出: 本身是箭头访问最内层链节的成员, 取指针操作数的值
  // (其指向的对象就是该对象); 其他每个成员 - 普通 `.` 访问, 或
  // 埋在展平匿名链更深处的箭头访问 - 取操作数的地址. 两种情形
  // 下临时变量都持有该对象, 所以 expr2/expr3 能在它上面指名
  // 成员; 槽位与构建出的形状就是 pre-B 降级产生的那些.
  if (node->lhs->kind == ND_MEMBER) {
    LibcpNode* operand = node->lhs->lhs;
    bool arrow         = node->lhs->arrow_tok;
    LibcpObj* var   = new_lvar("", libcp_pointer_to(ctx, arrow ? operand->ty->base : operand->ty));
    LibcpNode* addr = arrow ? operand : libcp_new_unary(ctx, ND_ADDR, operand, tok);

    LibcpNode* expr1 =
        libcp_new_binary(ctx, ND_ASSIGN, libcp_new_var_node(ctx, var, tok), addr, tok);

    LibcpNode* expr2 =
        libcp_new_unary(ctx,
                        ND_MEMBER,
                        libcp_new_unary(ctx, ND_DEREF, libcp_new_var_node(ctx, var, tok), tok),
                        tok);
    expr2->member = node->lhs->member;

    LibcpNode* expr3 =
        libcp_new_unary(ctx,
                        ND_MEMBER,
                        libcp_new_unary(ctx, ND_DEREF, libcp_new_var_node(ctx, var, tok), tok),
                        tok);
    expr3->member = node->lhs->member;

    LibcpNode* expr4 =
        libcp_new_binary(ctx, ND_ASSIGN, expr2, compound_op(ctx, node, expr3, tok), tok);

    return libcp_new_binary(ctx, ND_COMMA, expr1, expr4, tok);
  }

  // 若 A 是原子类型, 把 `A op= B` 转换成
  //
  // ({
  //   T1 *addr = &A; T2 val = (B); T1 old = *addr; T1 new;
  //   do {
  //    new = old op val;
  //   } while (!atomic_compare_exchange_strong(addr, &old, new));
  //   new;
  // })
  if (node->lhs->ty->is_atomic) {
    // `val` 临时变量持有按 `op` 要求处理过的操作数, 因此
    // `+=`/`-=` 的缩放在这里提前算出 - 通过 new_add/new_sub 完成,
    // 操作数检查也在那里运行.
    LibcpNode* rhs = node->rhs;
    if (node->op == ND_ADD || node->op == ND_SUB) {
      LibcpNode* operand = node->op == ND_ADD ? new_add(ctx, node->lhs, node->rhs, tok) :
                                                new_sub(ctx, node->lhs, node->rhs, tok);
      rhs                = operand->rhs;
    }
    libcp_add_type(ctx, rhs);

    LibcpNode head = {};
    LibcpNode* cur = &head;

    LibcpObj* addr = new_lvar("", libcp_pointer_to(ctx, node->lhs->ty));
    LibcpObj* val  = new_lvar("", rhs->ty);
    LibcpObj* old  = new_lvar("", node->lhs->ty);
    LibcpObj* new  = new_lvar("", node->lhs->ty);

    cur = cur->next =
        libcp_new_unary(ctx,
                        ND_EXPR_STMT,
                        libcp_new_binary(ctx,
                                         ND_ASSIGN,
                                         libcp_new_var_node(ctx, addr, tok),
                                         libcp_new_unary(ctx, ND_ADDR, node->lhs, tok),
                                         tok),
                        tok);

    cur = cur->next = libcp_new_unary(
        ctx,
        ND_EXPR_STMT,
        libcp_new_binary(ctx, ND_ASSIGN, libcp_new_var_node(ctx, val, tok), rhs, tok),
        tok);

    cur = cur->next = libcp_new_unary(
        ctx,
        ND_EXPR_STMT,
        libcp_new_binary(ctx,
                         ND_ASSIGN,
                         libcp_new_var_node(ctx, old, tok),
                         libcp_new_unary(ctx, ND_DEREF, libcp_new_var_node(ctx, addr, tok), tok),
                         tok),
        tok);

    LibcpNode* loop = libcp_new_node(ctx, ND_DO, tok);

    // 重试循环在这里取自己的标签, 而不经过 shape_node 下潜: 它
    // 的内部是 codegen 构建的降级形态, 没有需要整形的控制流 (内部
    // 没有循环, switch, break 或标签), 而下潜会对本 pass 刚构建的
    // 节点重复执行表达式分支 (contract 3(d) - 快照正好抓到这一点:
    // 循环体的操作数被转换了两次). 分配顺序 - 先 break 后 continue,
    // 且在到达外层语句的那个时点 - 与整树下潜会产生的顺序一致.
    cg(loop)->brk_label  = new_label(ctx);
    cg(loop)->cont_label = new_label(ctx);

    // `val` 已持有按 `op` 处理好的操作数 - 若 `op` 是指针上的加法
    // 运算符, 缩放已做 - 所以重试循环把它与旧值组合时不再缩放.
    LibcpNode* body = libcp_new_binary(ctx,
                                       ND_ASSIGN,
                                       libcp_new_var_node(ctx, new, tok),
                                       combine(ctx,
                                               node->op,
                                               libcp_new_var_node(ctx, old, tok),
                                               libcp_new_var_node(ctx, val, tok),
                                               tok),
                                       tok);

    loop->then       = libcp_new_node(ctx, ND_BLOCK, tok);
    loop->then->body = libcp_new_unary(ctx, ND_EXPR_STMT, body, tok);

    LibcpNode* cas = libcp_new_node(ctx, ND_CAS, tok);
    cas->cas_addr  = libcp_new_var_node(ctx, addr, tok);
    cas->cas_old   = libcp_new_unary(ctx, ND_ADDR, libcp_new_var_node(ctx, old, tok), tok);
    cas->cas_new   = libcp_new_var_node(ctx, new, tok);
    loop->cond     = libcp_new_unary(ctx, ND_NOT, cas, tok);

    cur = cur->next = loop;
    cur = cur->next = libcp_new_unary(ctx, ND_EXPR_STMT, libcp_new_var_node(ctx, new, tok), tok);

    LibcpNode* stmt_expr = libcp_new_node(ctx, ND_STMT_EXPR, tok);
    stmt_expr->body      = head.next;
    return stmt_expr;
  }

  // 把 `A op= B` 转换成 `tmp = &A, *tmp = *tmp op B`.
  LibcpObj* var = new_lvar("", libcp_pointer_to(ctx, node->lhs->ty));

  LibcpNode* expr1 = libcp_new_binary(ctx,
                                      ND_ASSIGN,
                                      libcp_new_var_node(ctx, var, tok),
                                      libcp_new_unary(ctx, ND_ADDR, node->lhs, tok),
                                      tok);

  LibcpNode* expr2 = libcp_new_binary(
      ctx,
      ND_ASSIGN,
      libcp_new_unary(ctx, ND_DEREF, libcp_new_var_node(ctx, var, tok), tok),
      compound_op(ctx,
                  node,
                  libcp_new_unary(ctx, ND_DEREF, libcp_new_var_node(ctx, var, tok), tok),
                  tok),
      tok);

  return libcp_new_binary(ctx, ND_COMMA, expr1, expr2, tok);
}

// 把 A++ 转换成 `(typeof A)((A += 1) - 1)`. 从 parse.c 移来.
static LibcpNode* new_inc_dec(LibcpContext* ctx, LibcpNode* node, LibcpToken* tok, int addend) {
  libcp_add_type(ctx, node);
  LibcpNode* expr = libcp_new_binary(ctx, ND_ASSIGN, node, libcp_new_num(ctx, addend, tok), tok);
  expr->op        = ND_ADD;
  LibcpNode* sub  = new_add(ctx, to_assign(ctx, expr), libcp_new_num(ctx, -addend, tok), tok);
  return libcp_new_cast(ctx, sub, node->ty);
}

// 下潜是前序的: 循环或 switch 在访问其主体之前先取得自己的标签.
// 标签存在节点上, 发射器在发射时针对最内层标签解析 break 或
// continue (PLAN B1.2a); 这里的下潜只保证每个循环都有它们.
static void shape_node(LibcpContext* ctx, LibcpNode* node) {
  if (!node)
    return;

  switch (node->kind) {
    case ND_WHILE:
    case ND_DO:
    case ND_FOR: {
      cg(node)->brk_label  = new_label(ctx);
      cg(node)->cont_label = new_label(ctx);

      shape_children(ctx, node);

      // gen_stmt 的 `for` 只有一个 init 语句槽, 所以展开留下多于
      // 一条语句的 init 链在这里归约成一个块; 没有 init 时无事可做
      // (PLAN A8.1, 从 sema 移来).
      if (node->kind == ND_FOR) {
        if (!node->init) {
          // 无需处理
        } else if (node->init->next) {
          LibcpNode* blk = libcp_new_node(ctx, ND_BLOCK, node->init->tok);
          blk->body      = node->init;
          node->init     = blk;
        }
      }
      return;
    }
    case ND_SWITCH: {
      cg(node)->brk_label = new_label(ctx);

      LibcpNode* sw  = current_switch;
      current_switch = node;
      shape_children(ctx, node);
      current_switch = sw;
      return;
    }
    case ND_CASE:
      cg(node)->unique_label = new_label(ctx);
      shape_node(ctx, node->lhs);

      // 在主体之后链接: 一个 case 标签可能位于更早 case 的语句内
      // (`case 0: while (..) { .. case 1: .. }`), 而链是前插的, 所以
      // 正是这个顺序让外层 case 在下面的比较链中排在埋入的那个之前.
      if (node->is_default) {
        cg(current_switch)->default_case = node;
      } else {
        cg(node)->case_next           = cg(current_switch)->case_next;
        cg(current_switch)->case_next = node;
      }
      return;
    case ND_GOTO:
      // 与函数收集的引用一起在下面解析, 不在这里: sema 把 goto 与
      // 标签值的收集放在 LibcpObj 上, 因为标签值可能位于语句树之外.
      return;
    case ND_LABEL:
      cg(node)->unique_label = new_label(ctx);
      libcp_nodearray_push(ctx, &cg_labels, node);
      shape_node(ctx, node->lhs);
      return;
    case ND_ASSIGN:
      shape_children(ctx, node);
      if (node->op) {
        // 解析器记录的复合赋值: 改写成读-改-写形态 (处理成员与原子
        // 情形), 然后重新给结果定型. 改写替换整个节点, 因为原子形态
        // 变成语句表达式, 它的主体在 lhs/rhs 之外.
        LibcpNode* result = to_assign(ctx, node);
        LibcpNode* nxt    = node->next;
        *node             = *result;
        node->next        = nxt;
        libcp_add_type(ctx, node);
      }
      return;
    case ND_INCDEC: {
      // 把 `++i`/`i--` 降级成复合赋值形态, 原地改写节点.
      shape_children(ctx, node);
      LibcpNode* operand = node->lhs;
      LibcpToken* tok    = node->tok;

      if (node->is_post) {
        // 结果是包在 `(A += 1) - 1` 外面并恢复操作数类型的隐式转换,
        // 所以它的标记必须随它贡献的形状一起传递.
        LibcpNode* result = new_inc_dec(ctx, operand, tok, node->addend);
        node->kind        = result->kind;
        node->lhs         = result->lhs;
        node->ty          = result->ty;
        node->is_implicit = result->is_implicit;
        return;
      }

      LibcpNode* expr = libcp_new_binary(ctx, ND_ASSIGN, operand, libcp_new_num(ctx, 1, tok), tok);
      expr->op        = node->addend < 0 ? ND_SUB : ND_ADD;
      LibcpNode* result = to_assign(ctx, expr);
      LibcpNode* nxt    = node->next;
      *node             = *result;
      node->next        = nxt;
      libcp_add_type(ctx, node);
      return;
    }
    case ND_FUNCALL:
      // 调用者的返回缓冲区是消费者的一个槽 (PLAN A6.1): sema 已检查
      // 被调者, 转换实参并记录返回类型; 返回类型为 struct 或 union 时,
      // 该调用需要缓冲区, 在此创建, 使其在 assign_lvar_offsets 运行前
      // 落入栈帧.
      shape_children(ctx, node);
      if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
        cg(node)->ret_buffer = new_lvar("", node->ty);
      return;
    case ND_COND:
      // 两种写法都保持自己的形态: GNU 的 `a ?: b` 在 gen_expr 中直接
      // 由自身字段无槽发射 (PLAN B1.1e), 普通条件表达式的分支已由
      // sema 转换.
      shape_children(ctx, node);
      return;
    case ND_SIZEOF: {
      // VLA 的 sizeof 是唯一结论为运行期值的情形: 把它降级为计算并
      // 读取尺寸变量的逗号表达式. 其它 sizeof 都以 `val` 中已得出的
      // 结论到达, 且任何形式都不求值其操作数, 所以两种情况都没有需要
      // 继续整形的下层.
      LibcpType* ty = node->ty_op ? node->ty_op : node->lhs->ty;
      if (ty->kind == TY_VLA) {
        // 类型名操作数携带一个新造的类型, 而链马上要嵌入的维度表达式
        // 从未经过这个遍 - 没有语句树能抵达它们. 先整形它们, 从最内层
        // 开始, 即尺寸计算遍历的顺序. 声明的类型不动: 其维度位于该声明
        // 自己的已降级链中, 下潜会在那里抵达它们 (声明标注恰好给这些
        // 类型设置了 vla_size, 这就是两种情形的区别).
        if (!cg_type(ty)->vla_size)
          for (LibcpType* t = ty; t; t = t->base)
            if (t->kind == TY_VLA)
              shape_node(ctx, t->vla_len);

        LibcpNode* expr = vla_size_expr(ctx, ty, node->tok);
        libcp_add_type(ctx, expr);
        LibcpNode* nxt = node->next;
        *node          = *expr;
        node->next     = nxt;
      }
      return;
    }
    case ND_GENERIC:
      // 只有被选中关联的表达式会进入发射; 控制表达式与未选中的分支只
      // 被定型, 不产生代码, 所以只整形选中的那一项.
      shape_node(ctx, node->generic_sel);
      return;
    case ND_COMPOUND_LITERAL:
      // 块作用域的复合字面量: 初始化器逗号链加对隐藏对象的引用
      // (PLAN A8.1; 对象本身由 sema 的 resolve pass 创建, 语言规定它
      // 在那里存在). 文件作用域的情形在 sema 中变成普通引用, 其数据由
      // sema 的序列化器负责.
      {
        LibcpObj* var = node->var;

        shape_init_exprs(ctx, node->init_resolved);
        LibcpNode* lowered = lvar_init_comma(ctx, var, node->init_resolved, node->decl_init->tok);
        LibcpNode* comma   = libcp_new_binary(ctx,
                                            ND_COMMA,
                                            lowered,
                                            libcp_new_var_node(ctx, var, node->tok),
                                            node->tok);
        libcp_add_type(ctx, comma);
        LibcpNode* nxt = node->next;
        *node          = *comma;
        node->next     = nxt;
      }
      return;
    default: shape_children(ctx, node); return;
  }
}

// 把函数收集的引用 (sema 的检查遍把它们存放在 context 中) 与上面下潜
// 分配的标签配对, 写入每个引用要跳转到的名字. 未声明标签的检查已在
// sema 中运行, 所以这里的每个引用都在那里匹配上了.
static void resolve_label_refs(LibcpContext* ctx, LibcpObj* fn) {
  LibcpNodeArray* refs = libcp_sema_label_gotos(ctx, fn);
  if (!refs)
    return;
  for (int i = 0; i < refs->len; i++) {
    LibcpNode* x = refs->data[i];
    for (int j = 0; j < cg_labels.len; j++) {
      LibcpNode* y = cg_labels.data[j];
      if (!strcmp(x->label, y->label)) {
        cg(x)->unique_label = cg(y)->unique_label;
        break;
      }
    }
  }
  cg_labels.len = 0;
}

// 整形每个已定义函数的函数体. 遍历沿 prog 进行, 因此 [GNU] 嵌套函数的
// 函数体 - 它也挂在宿主函数体内, 直到本遍的 shape_chain 把该记录摘除 -
// 恰好被访问一次, 即通过这里它自己的条目.
static void shape(LibcpContext* ctx, LibcpObj* prog) {
  for (LibcpObj* fn = prog; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition)
      continue;
    // 下面创建的改写临时变量进入该函数的栈帧, 所以槽工厂需要知道
    // 追加到哪里.
    current_fn = fn;
    shape_chain(ctx, &fn->body);
    resolve_label_refs(ctx, fn);
  }
}

// 给局部变量分配偏移.
static void assign_lvar_offsets(LibcpObj* prog) {
  for (LibcpObj* fn = prog; fn; fn = fn->next) {
    if (!fn->is_function)
      continue;

    // 函数的参数很多时, 有些参数不可避免地通过栈而非寄存器传递.
    // 第一个通过栈传递的参数位于 RBP+16.
    int top    = 16;
    int bottom = 0;

    int gp = 0, fp = 0;

    // 给通过栈传递的参数分配偏移.
    for (LibcpObj* var = fn->params; var; var = var->next) {
      LibcpType* ty = var->ty;

      switch (ty->kind) {
        case TY_STRUCT:
        case TY_UNION:
          if (ty->size <= 16) {
            bool fp1 = has_flonum(ty, 0, 8, 0);
            bool fp2 = has_flonum(ty, 8, 16, 8);
            if (fp + fp1 + fp2 < FP_MAX && gp + !fp1 + !fp2 < GP_MAX) {
              fp = fp + fp1 + fp2;
              gp = gp + !fp1 + !fp2;
              continue;
            }
          }
          break;
        case TY_FLOAT:
        case TY_DOUBLE:
          if (fp++ < FP_MAX)
            continue;
          break;
        case TY_LDOUBLE: break;
        default:
          if (gp++ < GP_MAX)
            continue;
      }

      top                 = libcp_align_to(top, 8);
      cg_obj(var)->offset = top;
      top += var->ty->size;
    }

    // 给通过寄存器传递的参数与局部变量分配偏移.
    for (LibcpObj* var = fn->locals; var; var = var->next) {
      if (obj_offset(var))
        continue;

      // AMD64 System V ABI 对长度至少 16 字节的数组有特殊对齐规则:
      // 这类数组至少要对齐到 16 字节边界. 见第 14 页:
      // https://github.com/hjl-tools/x86-psABI/wiki/x86-64-psABI-draft.pdf
      int align =
          (var->ty->kind == TY_ARRAY && var->ty->size >= 16) ? MAX(16, var->align) : var->align;

      bottom += var->ty->size;
      bottom              = libcp_align_to(bottom, align);
      cg_obj(var)->offset = -bottom;
    }

    cg_obj(fn)->stack_size = libcp_align_to(bottom, 16);
  }
}

static void emit_data(LibcpObj* prog) {
  for (LibcpObj* var = prog; var; var = var->next) {
    if (var->is_function || !var->is_definition)
      continue;

    if (var->is_static)
      println("  .local %s", var->name);
    else
      println("  .globl %s", var->name);

    int align =
        (var->ty->kind == TY_ARRAY && var->ty->size >= 16) ? MAX(16, var->align) : var->align;

    // common 符号
    if (opt_fcommon && var->is_tentative) {
      println("  .comm %s, %d, %d", var->name, var->ty->size, align);
      continue;
    }

    // .data 或 .tdata
    if (var->init_data) {
      if (var->is_tls)
        println("  .section .tdata,\"awT\",@progbits");
      else
        println("  .data");

      println("  .type %s, @object", var->name);
      println("  .size %s, %d", var->name, var->ty->size);
      println("  .align %d", align);
      println("%s:", var->name);

      LibcpRelocation* rel = var->rel;
      int pos              = 0;
      while (pos < var->ty->size) {
        if (rel && rel->offset == pos) {
          println("  .quad %s%+ld",
                  rel->label ? rel->label : cg(rel->label_node)->unique_label,
                  rel->addend);
          rel = rel->next;
          pos += 8;
          continue;
        }

        // 用能容纳的最宽伪指令发射一段普通字节, 在下一个重定位处停下.
        // 唯一目标是 little-endian x86-64, 所以一段字节按字节打包进
        // 该伪指令的值 (C7; 以前是每个字节一条 .byte).
        int len      = (rel ? rel->offset : var->ty->size) - pos;
        uint64_t val = 0;

        if (len >= 8) {
          for (int i = 0; i < 8; i++)
            val |= (uint64_t)(unsigned char)var->init_data[pos + i] << (i * 8);
          println("  .quad 0x%llx", (unsigned long long)val);
          pos += 8;
        } else if (len >= 4) {
          for (int i = 0; i < 4; i++)
            val |= (uint64_t)(unsigned char)var->init_data[pos + i] << (i * 8);
          println("  .long 0x%x", (unsigned)val);
          pos += 4;
        } else if (len >= 2) {
          for (int i = 0; i < 2; i++)
            val |= (uint64_t)(unsigned char)var->init_data[pos + i] << (i * 8);
          println("  .short 0x%x", (unsigned)val);
          pos += 2;
        } else {
          println("  .byte %d", (unsigned char)var->init_data[pos]);
          pos++;
        }
      }
      continue;
    }

    // .bss 或 .tbss
    if (var->is_tls)
      println("  .section .tbss,\"awT\",@nobits");
    else
      println("  .bss");

    println("  .align %d", align);
    println("%s:", var->name);
    println("  .zero %d", var->ty->size);
  }
}

static void store_fp(LibcpContext* ctx, int r, int offset, int sz) {
  switch (sz) {
    case 4: println("  movss %%xmm%d, %d(%%rbp)", r, offset); return;
    case 8: println("  movsd %%xmm%d, %d(%%rbp)", r, offset); return;
  }
  unreachable();
}

static void store_gp(int r, int offset, int sz) {
  switch (sz) {
    case 1: println("  mov %s, %d(%%rbp)", argreg8[r], offset); return;
    case 2: println("  mov %s, %d(%%rbp)", argreg16[r], offset); return;
    case 4: println("  mov %s, %d(%%rbp)", argreg32[r], offset); return;
    case 8: println("  mov %s, %d(%%rbp)", argreg64[r], offset); return;
    default:
      for (int i = 0; i < sz; i++) {
        println("  mov %s, %d(%%rbp)", argreg8[r], offset + i);
        println("  shr $8, %s", argreg64[r]);
      }
      return;
  }
}

static void emit_text(LibcpContext* ctx, LibcpObj* prog) {
  for (LibcpObj* fn = prog; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition)
      continue;

    // 没有人引用的 "static inline" 函数不发射代码.
    if (!fn->is_live)
      continue;

    if (fn->is_static)
      println("  .local %s", fn->name);
    else
      println("  .globl %s", fn->name);

    println("  .text");
    println("  .type %s, @function", fn->name);
    println("%s:", fn->name);
    current_fn = fn;

    // 序言
    println("  push %%rbp");
    println("  mov %%rsp, %%rbp");
    println("  sub $%d, %%rsp", obj_stack_size(fn));
    println("  mov %%rsp, %d(%%rbp)", obj_offset(fn->alloca_bottom));

    // 函数为可变参数时保存参数寄存器
    if (fn->va_area) {
      int gp = 0, fp = 0;
      for (LibcpObj* var = fn->params; var; var = var->next) {
        if (libcp_is_flonum(var->ty))
          fp++;
        else
          gp++;
      }

      int off = obj_offset(fn->va_area);

      // va_elem
      println("  movl $%d, %d(%%rbp)", gp * 8, off);           // gp_offset
      println("  movl $%d, %d(%%rbp)", fp * 8 + 48, off + 4);  // fp_offset
      println("  movq %%rbp, %d(%%rbp)", off + 8);             // overflow_arg_area
      println("  addq $16, %d(%%rbp)", off + 8);
      println("  movq %%rbp, %d(%%rbp)", off + 16);  // reg_save_area
      println("  addq $%d, %d(%%rbp)", off + 24, off + 16);

      // __reg_save_area__
      println("  movq %%rdi, %d(%%rbp)", off + 24);
      println("  movq %%rsi, %d(%%rbp)", off + 32);
      println("  movq %%rdx, %d(%%rbp)", off + 40);
      println("  movq %%rcx, %d(%%rbp)", off + 48);
      println("  movq %%r8, %d(%%rbp)", off + 56);
      println("  movq %%r9, %d(%%rbp)", off + 64);
      println("  movsd %%xmm0, %d(%%rbp)", off + 72);
      println("  movsd %%xmm1, %d(%%rbp)", off + 80);
      println("  movsd %%xmm2, %d(%%rbp)", off + 88);
      println("  movsd %%xmm3, %d(%%rbp)", off + 96);
      println("  movsd %%xmm4, %d(%%rbp)", off + 104);
      println("  movsd %%xmm5, %d(%%rbp)", off + 112);
      println("  movsd %%xmm6, %d(%%rbp)", off + 120);
      println("  movsd %%xmm7, %d(%%rbp)", off + 128);
    }

    // 把通过寄存器传递的参数保存到栈上
    int gp = 0, fp = 0;
    for (LibcpObj* var = fn->params; var; var = var->next) {
      if (obj_offset(var) > 0)
        continue;

      LibcpType* ty = var->ty;

      switch (ty->kind) {
        case TY_STRUCT:
        case TY_UNION:
          assert(ty->size <= 16);
          if (has_flonum(ty, 0, 8, 0))
            store_fp(ctx, fp++, obj_offset(var), MIN(8, ty->size));
          else
            store_gp(gp++, obj_offset(var), MIN(8, ty->size));

          if (ty->size > 8) {
            if (has_flonum(ty, 8, 16, 0))
              store_fp(ctx, fp++, obj_offset(var) + 8, ty->size - 8);
            else
              store_gp(gp++, obj_offset(var) + 8, ty->size - 8);
          }
          break;
        case TY_FLOAT:
        case TY_DOUBLE: store_fp(ctx, fp++, obj_offset(var), ty->size); break;
        default: store_gp(gp++, obj_offset(var), ty->size);
      }
    }

    // 发射代码
    gen_stmt(ctx, fn->body);
    assert(depth == 0);

    // [https://www.sigbus.info/n1570#5.1.2.2.3p1] C 标准为 main 函数
    // 定义了特殊规则: 到达 main 函数末尾等价于返回 0, 尽管其它函数
    // 的同样行为是未定义的.
    if (strcmp(fn->name, "main") == 0)
      println("  mov $0, %%rax");

    // 尾声
    println(".L.return.%s:", fn->name);
    println("  mov %%rbp, %%rsp");
    println("  pop %%rbp");
    println("  ret");
  }
}

void codegen(LibcpContext* ctx, LibcpObj* prog, FILE* out) {
  output_file = out;

  LibcpFile** files = libcp_get_input_files(ctx);
  for (int i = 0; files[i]; i++)
    println("  .file %d \"%s\"", files[i]->file_no, files[i]->name);

  // 整形遍最先运行: 它填充 gen_stmt 读取的标签字段并解析 goto 引用,
  // 在它之前不得进行任何分配或发射 (PLAN 契约 3).
  label_base = libcp_unique_name_next(ctx);
  shape(ctx, prog);

  assign_lvar_offsets(prog);
  emit_data(prog);
  emit_text(ctx, prog);
}
