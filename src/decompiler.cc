/*
 * Copyright 2019 WebAssembly Community Group participants
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

#include "src/decompiler.h"

#include "src/decompiler-ast.h"
#include "src/decompiler-ls.h"

#include "src/stream.h"

#define WABT_TRACING 0
#include "src/tracing.h"

#include <sstream>
#include <inttypes.h>

namespace wabt {

struct Decompiler {
  Decompiler(Stream& stream, const Module& module,
             const DecompileOptions& options)
      : mc(module), stream(stream), options(options) {}

  struct Value {
    std::vector<std::string> v;
    // Lazily add bracketing only if the parent requires it.
    // TODO: replace with a system based on precedence?
    bool needs_bracketing;

    Value(std::vector<std::string>&& v, bool nb)
      : v(v), needs_bracketing(nb) {}

    size_t width() {
      size_t w = 0;
      for (auto &s : v) {
        w = std::max(w, s.size());
      }
      return w;
    }

    // This value should really never be copied, only moved.
    Value(Value&& rhs) = default;
    Value(const Value& rhs) = delete;
    Value& operator=(Value&& rhs) = default;
    Value& operator=(const Value& rhs) = delete;
  };

  std::string Indent(size_t amount) {
    return std::string(amount, ' ');
  }

  string_view OpcodeToToken(Opcode opcode) {
    return opcode.GetDecomp();
  }

  void IndentValue(Value &val, size_t amount, string_view first_indent) {
    WABT_TRACE_ARGS(IndentValue, "\"" PRIstringview "\" - %d",
                    WABT_PRINTF_STRING_VIEW_ARG(val.v[0]), val.v.size());
    auto indent = Indent(amount);
    for (auto& s : val.v) {
      auto is = (&s != &val.v[0] || first_indent.empty())
                ? string_view(indent)
                : first_indent;
      s.insert(0, is.data(), is.size());
    }
  }

  Value WrapChild(Value &child, string_view prefix, string_view postfix) {
    WABT_TRACE_ARGS(WrapChild, "\"" PRIstringview "\" - \"" PRIstringview "\"",
                    WABT_PRINTF_STRING_VIEW_ARG(prefix),
                    WABT_PRINTF_STRING_VIEW_ARG(postfix));
    auto width = prefix.size() + postfix.size() + child.width();
    auto &v = child.v;
    if (width < target_exp_width ||
        (prefix.size() <= indent_amount && postfix.size() <= indent_amount)) {
      if (v.size() == 1) {
        // Fits in a single line.
        v[0].insert(0, prefix.data(), prefix.size());
        v[0].append(postfix.data(), postfix.size());
      } else {
        // Multiline, but with prefix on same line.
        IndentValue(child, prefix.size(), prefix);
        v.back().append(postfix.data(), postfix.size());
      }
    } else {
      // Multiline with prefix on its own line.
      IndentValue(child, indent_amount, {});
      v.insert(v.begin(), std::string(prefix));
      v.back().append(postfix.data(), postfix.size());
    }
    return std::move(child);
  }

  void BracketIfNeeded(Value &val) {
    if (!val.needs_bracketing) return;
    val = WrapChild(val, "(", ")");
    val.needs_bracketing = false;
  }

  Value WrapBinary(std::vector<Value>& args, string_view infix, bool indent_right) {
    assert(args.size() == 2);
    auto& left = args[0];
    auto& right = args[1];
    BracketIfNeeded(left);
    BracketIfNeeded(right);
    auto width = infix.size() + left.width() + right.width();
    if (width < target_exp_width && left.v.size() == 1 && right.v.size() == 1) {
      return Value {
        { left.v[0] + std::string(infix) + right.v[0] },
        true
      };
    } else {
      Value bin { {}, true };
      std::move(left.v.begin(), left.v.end(), std::back_inserter(bin.v));
      bin.v.back().append(infix.data(), infix.size());
      if (indent_right) IndentValue(right, indent_amount, {});
      std::move(right.v.begin(), right.v.end(), std::back_inserter(bin.v));
      return bin;
    }
  }

  Value WrapNAry(std::vector<Value>& args, string_view prefix, string_view postfix) {
    size_t total_width = 0;
    size_t max_width = 0;
    bool multiline = false;
    for (auto& child : args) {
      auto w = child.width();
      max_width = std::max(max_width, w);
      total_width += w;
      multiline = multiline || child.v.size() > 1;
    }
    if (!multiline &&
        (total_width + prefix.size() + postfix.size() < target_exp_width ||
         args.empty())) {
      // Single line.
      ss << prefix;
      for (auto& child : args) {
        if (&child != &args[0]) ss << ", ";
        ss << child.v[0];
      }
      ss << postfix;
      return PushSStream();
    } else {
      // Multi-line.
      Value ml { {}, false };
      auto ident_with_name = max_width + prefix.size() < target_exp_width;
      size_t i = 0;
      for (auto& child : args) {
        IndentValue(child, ident_with_name ? prefix.size() : indent_amount,
                    !i && ident_with_name ? prefix : string_view {});
        if (i < args.size() - 1) child.v.back() += ",";
        std::move(child.v.begin(), child.v.end(),
                  std::back_inserter(ml.v));
        i++;
      }
      if (!ident_with_name) ml.v.insert(ml.v.begin(), std::string(prefix));
      ml.v.back() += std::string(postfix);
      return ml;
    }
  }

  Value PushSStream() {
    auto v = Value { { ss.str() }, false };
    ss.str({});
    return v;
  }

  template<ExprType T> Value Get(const VarExpr<T>& ve) {
    ss << ve.var.name();
    return PushSStream();
  }

  template<ExprType T> Value Set(Value& child, const VarExpr<T>& ve) {
    child.needs_bracketing = true;
    return WrapChild(child, ve.var.name() + " = ", "");
  }

  Value Block(Value &val, const Block& block, LabelType label, const char *name) {
    IndentValue(val, indent_amount, {});
    val.v.insert(val.v.begin(), std::string(name) + " " + block.label + " {");
    val.v.push_back("}");
    return std::move(val);
  }

  std::string TempVarName(Index n) {
    // FIXME: this needs much better variable naming. Problem is, the code
    // in generate-names.cc has allready run, its dictionaries deleted, so it
    // is not easy to integrate with it.
    return "t" + std::to_string(n);
  }

  std::string LocalDecl(const std::string& name, Type t) {
    std::string s = name;
    s += ":";
    auto struc = lst.GenStruct(name);
    if (struc.empty()) {
      s += GetDecompTypeName(t);
    } else {
      s += struc;
    }
    return s;
  }

  void LoadStore(Value &val, const Node& addr_exp, uint32_t offset,
                  Opcode opc, Address align, Type op_type) {
    BracketIfNeeded(val);
    auto access = lst.GenAccess(offset, addr_exp);
    if (!access.empty()) {
      val.v.back() += ".";
      val.v.back() += access;
    } else {
      std::string suffix = "[";
      suffix += std::to_string(offset);
      suffix += "]:";
      suffix += GetDecompTypeName(GetMemoryType(op_type, opc));
      if (!opc.IsNaturallyAligned(align)) suffix += "@" + std::to_string(align);
      val.v.back() += suffix;
    }
  }

  Value DecompileExpr(const Node &n) {
    std::vector<Value> args;
    for (auto &c : n.children) {
      args.push_back(DecompileExpr(c));
    }
    // First deal with the specialized node types.
    switch (n.ntype) {
      case NodeType::FlushToVars: {
        std::string decls = "let ";
        for (Index i = 0; i < n.var_count; i++) {
          if (i) decls += ", ";
          decls += TempVarName(n.var_start + i);
        }
        decls += " = ";
        return WrapNAry(args, decls, "");
      }
      case NodeType::FlushedVar: {
        return Value { { TempVarName(n.var_start) }, false };
      }
      case NodeType::Statements: {
        Value stats { {}, false };
        for (size_t i = 0; i < n.children.size(); i++) {
          auto& s = args[i].v.back();
          if (s.back() != '}') s += ';';
          std::move(args[i].v.begin(), args[i].v.end(),
                    std::back_inserter(stats.v));
        }
        return stats;
      }
      case NodeType::EndReturn: {
        return WrapNAry(args, "return ", "");
      }
      case NodeType::Decl: {
        ss << "var ";
        ss << LocalDecl(n.var->name(), cur_func->GetLocalType(*n.var));
        return PushSStream();
      }
      case NodeType::DeclInit: {
        return WrapChild(args[0],
            "var " + LocalDecl(n.var->name(), cur_func->GetLocalType(*n.var)) +
            " = ", "");
      }
      case NodeType::Expr:
        // We're going to fall thru to the second switch to deal with ExprType.
        break;
    }
    switch (n.etype) {
      case ExprType::Const: {
        auto& c = cast<ConstExpr>(n.e)->const_;
        switch (c.type) {
          case Type::I32:
            ss << static_cast<int32_t>(c.u32);
            break;
          case Type::I64:
            ss << static_cast<int64_t>(c.u64) << "L";
            break;
          case Type::F32: {
            float f;
            memcpy(&f, &c.f32_bits, sizeof(float));
            ss << f << "f";
            break;
          }
          case Type::F64: {
            double d;
            memcpy(&d, &c.f64_bits, sizeof(double));
            ss << d << "d";
            break;
          }
          case Type::V128:
            ss << "V128";  // FIXME
            break;
          default:
            WABT_UNREACHABLE;
        }
        return PushSStream();
      }
      case ExprType::LocalGet: {
        return Get(*cast<LocalGetExpr>(n.e));
      }
      case ExprType::GlobalGet: {
        return Get(*cast<GlobalGetExpr>(n.e));
      }
      case ExprType::LocalSet: {
        return Set(args[0], *cast<LocalSetExpr>(n.e));
      }
      case ExprType::GlobalSet: {
        return Set(args[0], *cast<GlobalSetExpr>(n.e));
      }
      case ExprType::LocalTee: {
        auto& te = *cast<LocalTeeExpr>(n.e);
        return args.empty() ? Get(te) : Set(args[0], te);
      }
      case ExprType::Binary: {
        auto& be = *cast<BinaryExpr>(n.e);
        return WrapBinary(args, " " + std::string(OpcodeToToken(be.opcode)) + " ", false);
      }
      case ExprType::Compare: {
        auto& ce = *cast<CompareExpr>(n.e);
        return WrapBinary(args, " " + std::string(OpcodeToToken(ce.opcode)) + " ", false);
      }
      case ExprType::Unary: {
        auto& ue = *cast<UnaryExpr>(n.e);
        //BracketIfNeeded(stack.back());
        // TODO: also version without () depending on precedence.
        return WrapChild(args[0],
                  std::string(OpcodeToToken(ue.opcode)) + "(", ")");
      }
      case ExprType::Load: {
        auto& le = *cast<LoadExpr>(n.e);
        LoadStore(args[0], n.children[0], le.offset, le.opcode, le.align,
                  le.opcode.GetResultType());
        return std::move(args[0]);
      }
      case ExprType::Store: {
        auto& se = *cast<StoreExpr>(n.e);
        LoadStore(args[0], n.children[0], se.offset, se.opcode, se.align,
                  se.opcode.GetParamType2());
        return WrapBinary(args, " = ", true);
      }
      case ExprType::If: {
        auto ife = cast<IfExpr>(n.e);
        Value *elsep = nullptr;
        if (!ife->false_.empty()) {
          elsep = &args[2];
        }
        auto& thenp = args[1];
        auto& ifs = args[0];
        bool multiline = ifs.v.size() > 1 || thenp.v.size() > 1;
        size_t width = ifs.width() + thenp.width();
        if (elsep) {
          width += elsep->width();
          multiline = multiline || elsep->v.size() > 1;
        }
        multiline = multiline || width > target_exp_width;
        if (multiline) {
          auto if_start = string_view("if (");
          ifs.v[0].insert(0, if_start.data(), if_start.size());
          ifs.v.back() += ") {";
          IndentValue(thenp, indent_amount, {});
          std::move(thenp.v.begin(), thenp.v.end(), std::back_inserter(ifs.v));
          if (elsep) {
            ifs.v.push_back("} else {");
            IndentValue(*elsep, indent_amount, {});
            std::move(elsep->v.begin(), elsep->v.end(), std::back_inserter(ifs.v));
          }
          ifs.v.push_back("}");
          return std::move(ifs);
        } else {
          ss << "if (" << ifs.v[0] << ") { " <<  thenp.v[0] << " }";
          if (elsep) ss << " else { " << elsep->v[0] << " }";
          return PushSStream();
        }
      }
      case ExprType::Block: {
        return Block(args[0], cast<BlockExpr>(n.e)->block, LabelType::Block, "block");
      }
      case ExprType::Loop: {
        return Block(args[0], cast<LoopExpr>(n.e)->block, LabelType::Loop, "loop");
      }
      case ExprType::Br: {
        auto be = cast<BrExpr>(n.e);
        auto jmp = n.lt == LabelType::Loop ? "continue" : "break";
        ss << jmp << " " << be->var.name();
        return PushSStream();
      }
      case ExprType::BrIf: {
        auto bie = cast<BrIfExpr>(n.e);
        auto jmp = n.lt == LabelType::Loop ? "continue" : "break";
        return WrapChild(args[0], "if (",
                  ") " + std::string(jmp) + " " + bie->var.name());
      }
      case ExprType::Return: {
        return WrapNAry(args, "return ", "");
      }
      case ExprType::Drop: {
        // Silent dropping of return values is very common, so currently
        // don't output this.
        return std::move(args[0]);
      }
      default: {
        std::string name;
        switch (n.etype) {
          case ExprType::Call:
            name = cast<CallExpr>(n.e)->var.name();
            break;
          case ExprType::Convert:
            name = std::string(OpcodeToToken(cast<ConvertExpr>(n.e)->opcode));
            break;
          default:
            name = GetExprTypeName(n.etype);
            break;
        }
        return WrapNAry(args, name + "(", ")");
      }
    }
  }

  bool CheckImportExport(ExternalKind kind, Index index, string_view name) {
    // Figure out if this thing is imported, exported, or neither.
    auto is_import = mc.module.IsImport(kind, Var(index));
    // TODO: is this the best way to check for export?
    // FIXME: this doesn't work for functions that get renamed in some way,
    // as the export has the original name..
    auto xport = mc.module.GetExport(name);
    auto is_export = xport && xport->kind == kind;
    if (is_export) stream.Writef("export ");
    if (is_import) stream.Writef("import ");
    return is_import;
  }

  std::string InitExp(const ExprList &el) {
    assert(!el.empty());
    AST ast(mc, nullptr);
    ast.Construct(el, 1, false);
    auto val = DecompileExpr(ast.exp_stack[0]);
    assert(ast.exp_stack.size() == 1 && val.v.size() == 1);
    return std::move(val.v[0]);
  }

  // FIXME: Merge with WatWriter::WriteQuotedData somehow.
  std::string BinaryToString(const std::vector<uint8_t> &in) {
    std::string s = "\"";
    static const char s_hexdigits[] = "0123456789abcdef";
    for (auto c : in) {
      if (c >= ' ' && c <= '~') {
        s += c;
      } else {
        s += '\\';
        s += s_hexdigits[c >> 4];
        s += s_hexdigits[c & 0xf];
      }
    }
    s += '\"';
    return s;
  }

  void Decompile() {
    // Memories.
    Index memory_index = 0;
    for (auto m : mc.module.memories) {
      auto is_import =
          CheckImportExport(ExternalKind::Memory, memory_index, m->name);
      stream.Writef("memory %s", m->name.c_str());
      if (!is_import) {
        stream.Writef("(initial: %" PRIu64 ", max: %" PRIu64 ")",
                      m->page_limits.initial, m->page_limits.max);
      }
      stream.Writef(";\n");
      memory_index++;
    }
    if (!mc.module.memories.empty()) stream.Writef("\n");

    // Globals.
    Index global_index = 0;
    for (auto g : mc.module.globals) {
      auto is_import =
          CheckImportExport(ExternalKind::Global, global_index, g->name);
      stream.Writef("global %s:%s", g->name.c_str(),
                    GetDecompTypeName(g->type));
      if (!is_import) {
        stream.Writef(" = %s", InitExp(g->init_expr).c_str());
      }
      stream.Writef(";\n");
      global_index++;
    }
    if (!mc.module.globals.empty()) stream.Writef("\n");

    // Tables.
    Index table_index = 0;
    for (auto tab : mc.module.tables) {
      auto is_import =
          CheckImportExport(ExternalKind::Table, table_index, tab->name);
      stream.Writef("table %s:%s", tab->name.c_str(),
                    GetDecompTypeName(tab->elem_type));
      if (!is_import) {
        stream.Writef("(min: %" PRIu64 ", max: %" PRIu64 ")",
                      tab->elem_limits.initial, tab->elem_limits.max);
      }
      stream.Writef(";\n");
      table_index++;
    }
    if (!mc.module.tables.empty()) stream.Writef("\n");

    // Data.
    for (auto dat : mc.module.data_segments) {
      stream.Writef("data %s(offset: %s) = %s;\n", dat->name.c_str(),
                    InitExp(dat->offset).c_str(),
                    BinaryToString(dat->data).c_str());
    }
    if (!mc.module.data_segments.empty()) stream.Writef("\n");

    // Code.
    Index func_index = 0;
    for (auto f : mc.module.funcs) {
      cur_func = f;
      auto is_import =
          CheckImportExport(ExternalKind::Func, func_index, f->name);
      AST ast(mc, f);
      if (!is_import) {
        ast.Construct(f->exprs, f->GetNumResults(), true);
        lst.Track(ast.exp_stack[0]);
        lst.CheckLayouts();
      }
      stream.Writef("function %s(", f->name.c_str());
      for (Index i = 0; i < f->GetNumParams(); i++) {
        if (i) stream.Writef(", ");
        auto t = f->GetParamType(i);
        auto name = IndexToAlphaName(i);
        stream.Writef("%s", LocalDecl(name, t).c_str());
      }
      stream.Writef(")");
      if (f->GetNumResults()) {
        if (f->GetNumResults() == 1) {
          stream.Writef(":%s", GetDecompTypeName(f->GetResultType(0)));
        } else {
          stream.Writef(":(");
          for (Index i = 0; i < f->GetNumResults(); i++) {
            if (i) stream.Writef(", ");
            stream.Writef("%s", GetDecompTypeName(f->GetResultType(i)));
          }
          stream.Writef(")");
        }
      }
      if (is_import) {
        stream.Writef(";");
      } else {
        stream.Writef(" {\n");
        auto val = DecompileExpr(ast.exp_stack[0]);
        IndentValue(val, indent_amount, {});
        for (auto& s : val.v) {
          stream.Writef("%s\n", s.c_str());
        }
        stream.Writef("}");
      }
      stream.Writef("\n\n");
      mc.EndFunc();
      lst.Clear();
      func_index++;
    }
  }

  void DumpInternalState() {
    // TODO: print ast?
    stream.Flush();
  }

  // For debugging purposes, this assert, when it fails, first prints the
  // internal state that hasn't been printed yet.
  void Assert(bool ok) {
    if (ok) return;
    #ifndef NDEBUG
      DumpInternalState();
    #endif
    assert(false);
  }

  ModuleContext mc;
  Stream& stream;
  const DecompileOptions& options;
  std::ostringstream ss;
  size_t indent_amount = 2;
  size_t target_exp_width = 70;
  const Func* cur_func = nullptr;
  LoadStoreTracking lst;
};

Result Decompile(Stream& stream,
                 const Module& module,
                 const DecompileOptions& options) {
  Decompiler decompiler(stream, module, options);
  decompiler.Decompile();
  return Result::Ok;
}

}  // namespace wabt
