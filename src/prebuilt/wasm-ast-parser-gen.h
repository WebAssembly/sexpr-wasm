/* A Bison parser, made by GNU Bison 3.0.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2013 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

#ifndef YY_WASM_AST_PARSER_SRC_PREBUILT_WASM_AST_PARSER_GEN_H_INCLUDED
# define YY_WASM_AST_PARSER_SRC_PREBUILT_WASM_AST_PARSER_GEN_H_INCLUDED
/* Debug traces.  */
#ifndef WASM_AST_PARSER_DEBUG
# if defined YYDEBUG
#if YYDEBUG
#   define WASM_AST_PARSER_DEBUG 1
#  else
#   define WASM_AST_PARSER_DEBUG 0
#  endif
# else /* ! defined YYDEBUG */
#  define WASM_AST_PARSER_DEBUG 0
# endif /* ! defined YYDEBUG */
#endif  /* ! defined WASM_AST_PARSER_DEBUG */
#if WASM_AST_PARSER_DEBUG
extern int wasm_ast_parser_debug;
#endif

/* Token type.  */
#ifndef WASM_AST_PARSER_TOKENTYPE
# define WASM_AST_PARSER_TOKENTYPE
  enum wasm_ast_parser_tokentype
  {
    WASM_TOKEN_TYPE_EOF = 0,
    WASM_TOKEN_TYPE_LPAR = 258,
    WASM_TOKEN_TYPE_RPAR = 259,
    WASM_TOKEN_TYPE_NAT = 260,
    WASM_TOKEN_TYPE_INT = 261,
    WASM_TOKEN_TYPE_FLOAT = 262,
    WASM_TOKEN_TYPE_TEXT = 263,
    WASM_TOKEN_TYPE_VAR = 264,
    WASM_TOKEN_TYPE_VALUE_TYPE = 265,
    WASM_TOKEN_TYPE_ANYFUNC = 266,
    WASM_TOKEN_TYPE_NOP = 267,
    WASM_TOKEN_TYPE_DROP = 268,
    WASM_TOKEN_TYPE_BLOCK = 269,
    WASM_TOKEN_TYPE_END = 270,
    WASM_TOKEN_TYPE_IF = 271,
    WASM_TOKEN_TYPE_THEN = 272,
    WASM_TOKEN_TYPE_ELSE = 273,
    WASM_TOKEN_TYPE_LOOP = 274,
    WASM_TOKEN_TYPE_BR = 275,
    WASM_TOKEN_TYPE_BR_IF = 276,
    WASM_TOKEN_TYPE_BR_TABLE = 277,
    WASM_TOKEN_TYPE_CALL = 278,
    WASM_TOKEN_TYPE_CALL_IMPORT = 279,
    WASM_TOKEN_TYPE_CALL_INDIRECT = 280,
    WASM_TOKEN_TYPE_RETURN = 281,
    WASM_TOKEN_TYPE_GET_LOCAL = 282,
    WASM_TOKEN_TYPE_SET_LOCAL = 283,
    WASM_TOKEN_TYPE_TEE_LOCAL = 284,
    WASM_TOKEN_TYPE_GET_GLOBAL = 285,
    WASM_TOKEN_TYPE_SET_GLOBAL = 286,
    WASM_TOKEN_TYPE_LOAD = 287,
    WASM_TOKEN_TYPE_STORE = 288,
    WASM_TOKEN_TYPE_OFFSET_EQ_NAT = 289,
    WASM_TOKEN_TYPE_ALIGN_EQ_NAT = 290,
    WASM_TOKEN_TYPE_CONST = 291,
    WASM_TOKEN_TYPE_UNARY = 292,
    WASM_TOKEN_TYPE_BINARY = 293,
    WASM_TOKEN_TYPE_COMPARE = 294,
    WASM_TOKEN_TYPE_CONVERT = 295,
    WASM_TOKEN_TYPE_SELECT = 296,
    WASM_TOKEN_TYPE_UNREACHABLE = 297,
    WASM_TOKEN_TYPE_CURRENT_MEMORY = 298,
    WASM_TOKEN_TYPE_GROW_MEMORY = 299,
    WASM_TOKEN_TYPE_FUNC = 300,
    WASM_TOKEN_TYPE_START = 301,
    WASM_TOKEN_TYPE_TYPE = 302,
    WASM_TOKEN_TYPE_PARAM = 303,
    WASM_TOKEN_TYPE_RESULT = 304,
    WASM_TOKEN_TYPE_LOCAL = 305,
    WASM_TOKEN_TYPE_GLOBAL = 306,
    WASM_TOKEN_TYPE_MODULE = 307,
    WASM_TOKEN_TYPE_TABLE = 308,
    WASM_TOKEN_TYPE_ELEM = 309,
    WASM_TOKEN_TYPE_MEMORY = 310,
    WASM_TOKEN_TYPE_DATA = 311,
    WASM_TOKEN_TYPE_OFFSET = 312,
    WASM_TOKEN_TYPE_IMPORT = 313,
    WASM_TOKEN_TYPE_EXPORT = 314,
    WASM_TOKEN_TYPE_ASSERT_INVALID = 315,
    WASM_TOKEN_TYPE_ASSERT_RETURN = 316,
    WASM_TOKEN_TYPE_ASSERT_RETURN_NAN = 317,
    WASM_TOKEN_TYPE_ASSERT_TRAP = 318,
    WASM_TOKEN_TYPE_INVOKE = 319,
    WASM_TOKEN_TYPE_LOW = 320
  };
#endif

/* Value type.  */
#if ! defined WASM_AST_PARSER_STYPE && ! defined WASM_AST_PARSER_STYPE_IS_DECLARED
typedef WasmToken WASM_AST_PARSER_STYPE;
# define WASM_AST_PARSER_STYPE_IS_TRIVIAL 1
# define WASM_AST_PARSER_STYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined WASM_AST_PARSER_LTYPE && ! defined WASM_AST_PARSER_LTYPE_IS_DECLARED
typedef struct WASM_AST_PARSER_LTYPE WASM_AST_PARSER_LTYPE;
struct WASM_AST_PARSER_LTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define WASM_AST_PARSER_LTYPE_IS_DECLARED 1
# define WASM_AST_PARSER_LTYPE_IS_TRIVIAL 1
#endif



int wasm_ast_parser_parse (WasmAstLexer* lexer, WasmAstParser* parser);

#endif /* !YY_WASM_AST_PARSER_SRC_PREBUILT_WASM_AST_PARSER_GEN_H_INCLUDED  */
