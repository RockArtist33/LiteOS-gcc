/* CPP Library.
   Copyright (C) 1986, 1987, 1989, 1992, 1993, 1994, 1995, 1996, 1997, 1998,
   1999, 2000 Free Software Foundation, Inc.
   Contributed by Per Bothner, 1994-95.
   Based on CCCP program by Paul Rubin, June 1986
   Adapted to ANSI C, Richard Stallman, Jan 1987

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"

#include "cpplib.h"
#include "cpphash.h"
#include "intl.h"
#include "obstack.h"
#include "symcat.h"

/* Chained list of answers to an assertion.  */
struct answer
{
  struct answer *next;
  unsigned int count;
  cpp_token first[1];
};

/* Stack of conditionals currently in progress
   (including both successful and failing conditionals).  */

struct if_stack
{
  struct if_stack *next;
  cpp_lexer_pos pos;		/* line and column where condition started */
  const cpp_hashnode *mi_cmacro;/* macro name for #ifndef around entire file */
  int was_skipping;		/* value of pfile->skipping before this if */
  int type;			/* type of last directive seen in this group */
};

/* Values for the origin field of struct directive.  KANDR directives
   come from traditional (K&R) C.  STDC89 directives come from the
   1989 C standard.  EXTENSION directives are extensions.  */
#define KANDR		0
#define STDC89		1
#define EXTENSION	2

/* Values for the flags field of struct directive.  COND indicates a
   conditional; IF_COND an opening conditional.  INCL means to treat
   "..." and <...> as q-char and h-char sequences respectively.  IN_I
   means this directive should be handled even if -fpreprocessed is in
   effect (these are the directives with callback hooks).  */
#define COND		(1 << 0)
#define IF_COND		(1 << 1)
#define INCL		(1 << 2)
#define IN_I		(1 << 3)

/* Defines one #-directive, including how to handle it.  */
typedef void (*directive_handler) PARAMS ((cpp_reader *));
typedef struct directive directive;
struct directive
{
  directive_handler handler;	/* Function to handle directive.  */
  const U_CHAR *name;		/* Name of directive.  */
  unsigned short length;	/* Length of name.  */
  unsigned char origin;		/* Origin of directive.  */
  unsigned char flags;	        /* Flags describing this directive.  */
};

/* Forward declarations.  */

static void skip_rest_of_line	PARAMS ((cpp_reader *));
static void check_eol		PARAMS ((cpp_reader *));
static void run_directive	PARAMS ((cpp_reader *, int,
					 const char *, size_t,
					 const char *));
static int glue_header_name	PARAMS ((cpp_reader *, cpp_token *));
static int  parse_include	PARAMS ((cpp_reader *, cpp_token *));
static void push_conditional	PARAMS ((cpp_reader *, int, int,
					 const cpp_hashnode *));
static int  read_line_number	PARAMS ((cpp_reader *, int *));
static int  strtoul_for_line	PARAMS ((const U_CHAR *, unsigned int,
					 unsigned long *));
static void do_diagnostic	PARAMS ((cpp_reader *, enum error_type));
static cpp_hashnode *
	lex_macro_node		PARAMS ((cpp_reader *));
static void unwind_if_stack	PARAMS ((cpp_reader *, cpp_buffer *));
static void do_pragma_once	PARAMS ((cpp_reader *));
static void do_pragma_poison	PARAMS ((cpp_reader *));
static void do_pragma_system_header	PARAMS ((cpp_reader *));
static void do_pragma_dependency	PARAMS ((cpp_reader *));
static int get__Pragma_string	PARAMS ((cpp_reader *, cpp_token *));
static unsigned char *destringize	PARAMS ((const cpp_string *,
						 unsigned int *));
static int parse_answer PARAMS ((cpp_reader *, struct answer **, int));
static cpp_hashnode *parse_assertion PARAMS ((cpp_reader *, struct answer **,
					      int));
static struct answer ** find_answer PARAMS ((cpp_hashnode *,
					     const struct answer *));
static void handle_assertion	PARAMS ((cpp_reader *, const char *, int));

/* This is the table of directive handlers.  It is ordered by
   frequency of occurrence; the numbers at the end are directive
   counts from all the source code I have lying around (egcs and libc
   CVS as of 1999-05-18, plus grub-0.5.91, linux-2.2.9, and
   pcmcia-cs-3.0.9).  This is no longer important as directive lookup
   is now O(1).  All extensions other than #warning and #include_next
   are deprecated.  The name is where the extension appears to have
   come from.  */

#define DIRECTIVE_TABLE							\
D(define,	T_DEFINE = 0,	KANDR,     IN_I)	   /* 270554 */ \
D(include,	T_INCLUDE,	KANDR,     INCL)	   /*  52262 */ \
D(endif,	T_ENDIF,	KANDR,     COND)	   /*  45855 */ \
D(ifdef,	T_IFDEF,	KANDR,     COND | IF_COND) /*  22000 */ \
D(if,		T_IF,		KANDR,     COND | IF_COND) /*  18162 */ \
D(else,		T_ELSE,		KANDR,     COND)	   /*   9863 */ \
D(ifndef,	T_IFNDEF,	KANDR,     COND | IF_COND) /*   9675 */ \
D(undef,	T_UNDEF,	KANDR,     IN_I)	   /*   4837 */ \
D(line,		T_LINE,		KANDR,     IN_I)	   /*   2465 */ \
D(elif,		T_ELIF,		KANDR,     COND)	   /*    610 */ \
D(error,	T_ERROR,	STDC89,    0)		   /*    475 */ \
D(pragma,	T_PRAGMA,	STDC89,    IN_I)	   /*    195 */ \
D(warning,	T_WARNING,	EXTENSION, 0)		   /*     22 */ \
D(include_next,	T_INCLUDE_NEXT,	EXTENSION, INCL)	   /*     19 */ \
D(ident,	T_IDENT,	EXTENSION, IN_I)	   /*     11 */ \
D(import,	T_IMPORT,	EXTENSION, INCL)	   /* 0 ObjC */	\
D(assert,	T_ASSERT,	EXTENSION, 0)		   /* 0 SVR4 */	\
D(unassert,	T_UNASSERT,	EXTENSION, 0)		   /* 0 SVR4 */	\
SCCS_ENTRY						   /* 0 SVR4? */

/* #sccs is not always recognized.  */
#ifdef SCCS_DIRECTIVE
# define SCCS_ENTRY D(sccs, T_SCCS, EXTENSION, 0)
#else
# define SCCS_ENTRY /* nothing */
#endif

/* Use the table to generate a series of prototypes, an enum for the
   directive names, and an array of directive handlers.  */

/* The directive-processing functions are declared to return int
   instead of void, because some old compilers have trouble with
   pointers to functions returning void.  */

/* Don't invoke CONCAT2 with any whitespace or K&R cc will fail. */
#define D(name, t, o, f) static void CONCAT2(do_,name) PARAMS ((cpp_reader *));
DIRECTIVE_TABLE
#undef D

#define D(n, tag, o, f) tag,
enum
{
  T_BAD_DIRECTIVE,
  DIRECTIVE_TABLE
  N_DIRECTIVES
};
#undef D

/* Don't invoke CONCAT2 with any whitespace or K&R cc will fail. */
#define D(name, t, origin, flags) \
{ CONCAT2(do_,name), (const U_CHAR *) STRINGX(name), \
  sizeof STRINGX(name) - 1, origin, flags },
static const directive dtable[] =
{
DIRECTIVE_TABLE
};
#undef D
#undef DIRECTIVE_TABLE

/* Skip any remaining tokens in a directive.  */
static void
skip_rest_of_line (pfile)
     cpp_reader *pfile;
{
  cpp_token token;

  /* Discard all stacked contexts.  */
  while (pfile->context != &pfile->base_context)
    _cpp_pop_context (pfile);

  /* Sweep up all tokens remaining on the line.  We need to read
     tokens from lookahead, but cannot just drop the lookahead buffers
     because they may be saving tokens prior to this directive for an
     external client.  So we use _cpp_get_token, with macros disabled.  */
  pfile->state.prevent_expansion++;
  while (!pfile->state.skip_newlines)
    _cpp_get_token (pfile, &token);
  pfile->state.prevent_expansion--;
}

/* Ensure there are no stray tokens at the end of a directive.  */
static void
check_eol (pfile)
     cpp_reader *pfile;
{
  if (!pfile->state.skip_newlines)
    {
      cpp_token token;

      _cpp_lex_token (pfile, &token);
      if (token.type != CPP_EOF)
	cpp_pedwarn (pfile, "extra tokens at end of #%s directive",
		     pfile->directive->name);
    }
}

/* Check if a token's name matches that of a known directive.  Put in
   this file to save exporting dtable and other unneeded information.  */
int
_cpp_handle_directive (pfile, indented)
     cpp_reader *pfile;
     int indented;
{
  const directive *dir = 0;
  cpp_token dname;
  int not_asm = 1;

  /* Some handlers need the position of the # for diagnostics.  */
  pfile->directive_pos = pfile->lexer_pos;

  /* We're now in a directive.  This ensures we get pedantic warnings
     about /v and /f in whitespace.  */
  pfile->state.in_directive = 1;
  pfile->state.save_comments = 0;

  /* Lex the directive name directly.  */
  _cpp_lex_token (pfile, &dname);

  if (dname.type == CPP_NAME)
    {
      unsigned int index = dname.val.node->directive_index;
      if (index)
	dir = &dtable[index - 1];
    }
  else if (dname.type == CPP_NUMBER)
    {
      /* # followed by a number is equivalent to #line.  Do not
	 recognize this form in assembly language source files or
	 skipped conditional groups.  Complain about this form if
	 we're being pedantic, but not if this is regurgitated input
	 (preprocessed or fed back in by the C++ frontend).  */
      if (!pfile->skipping  && !CPP_OPTION (pfile, lang_asm))
	{
	  dir = &dtable[T_LINE];
	  _cpp_push_token (pfile, &dname, &pfile->directive_pos);
	  if (CPP_PEDANTIC (pfile) && CPP_BUFFER (pfile)->inc
	      && ! CPP_OPTION (pfile, preprocessed))
	    cpp_pedwarn (pfile, "# followed by integer");
	}
    }

  pfile->directive = dir;
  if (dir)
    {
      /* Make sure we lex headers correctly, whether skipping or not.  */
      pfile->state.angled_headers = dir->flags & INCL;

      /* If we are rescanning preprocessed input, only directives tagged
	 with IN_I are honored, and the warnings below are suppressed.  */
      if (! CPP_OPTION (pfile, preprocessed) || dir->flags & IN_I)
	{
	  /* Traditionally, a directive is ignored unless its # is in
	     column 1.  Therefore in code intended to work with K+R
	     compilers, directives added by C89 must have their #
	     indented, and directives present in traditional C must
	     not.  This is true even of directives in skipped
	     conditional blocks.  */
	  if (CPP_WTRADITIONAL (pfile))
	    {
	      if (indented && dir->origin == KANDR)
		cpp_warning (pfile,
			     "traditional C ignores #%s with the # indented",
			     dir->name);
	      else if (!indented && dir->origin != KANDR)
		cpp_warning (pfile,
	     "suggest hiding #%s from traditional C with an indented #",
			     dir->name);
	    }

	  /* If we are skipping a failed conditional group, all
	     non-conditional directives are ignored.  */
	  if (!pfile->skipping || (dir->flags & COND))
	    {
	      /* Issue -pedantic warnings for extensions.   */
	      if (CPP_PEDANTIC (pfile) && dir->origin == EXTENSION)
		cpp_pedwarn (pfile, "#%s is a GCC extension", dir->name);

	      /* If we have a directive that is not an opening
		 conditional, invalidate any control macro.  */
	      if (! (dir->flags & IF_COND))
		pfile->mi_state = MI_FAILED;

	      (*dir->handler) (pfile);
	    }
	}
    }
  else if (dname.type == CPP_EOF)
    {
      /* The null directive.  */
      if (indented && CPP_WTRADITIONAL (pfile))
	cpp_warning (pfile, "traditional C ignores #\\n with the # indented");
    }
  else
    {
      /* An unknown directive.  Don't complain about it in assembly
	 source: we don't know where the comments are, and # may
	 introduce assembler pseudo-ops.  Don't complain about invalid
	 directives in skipped conditional groups (6.10 p4).  */
      if (CPP_OPTION (pfile, lang_asm))
	{
	  /* Output the # and lookahead token for the assembler.  */
	  not_asm = 0;
	  _cpp_push_token (pfile, &dname, &pfile->directive_pos);
	}
      else if (!pfile->skipping)
	cpp_error (pfile, "invalid preprocessing directive #%s",
		   cpp_token_as_text (pfile, &dname));
    }

  /* Save the lookahead token for assembler.  */
  if (not_asm)
    skip_rest_of_line (pfile);
  pfile->state.save_comments = ! CPP_OPTION (pfile, discard_comments);
  pfile->state.in_directive = 0;
  pfile->state.angled_headers = 0;
  pfile->directive = 0;

  return not_asm;
}

/* Directive handler wrapper used by the command line option
   processor.  */
static void
run_directive (pfile, dir_no, buf, count, name)
     cpp_reader *pfile;
     int dir_no;
     const char *buf;
     size_t count;
     const char *name;
{
  if (cpp_push_buffer (pfile, (const U_CHAR *) buf, count) != NULL)
    {
      const struct directive *dir = &dtable[dir_no];

      if (name)
	CPP_BUFFER (pfile)->nominal_fname = name;
      else
	CPP_BUFFER (pfile)->nominal_fname = _("<command line>");

      /* A kludge to avoid line markers for _Pragma.  */
      if (dir_no == T_PRAGMA)
	pfile->lexer_pos.output_line = CPP_BUFFER (pfile)->prev->lineno;

      /* For _Pragma, the text is passed through preprocessing stage 3
	 only, i.e. no trigraphs, no escaped newline removal, and no
	 macro expansion.  Do the same for command-line directives.  */
      pfile->buffer->from_stage3 = 1;
      pfile->state.in_directive = 1;
      pfile->directive = dir;
      pfile->state.prevent_expansion++;
      (void) (*dir->handler) (pfile);
      pfile->state.prevent_expansion--;
      pfile->directive = 0;
      pfile->state.in_directive = 0;

      skip_rest_of_line (pfile);
      if (pfile->buffer->cur != pfile->buffer->rlimit)
	cpp_error (pfile, "extra text after end of #%s directive",
		   dtable[dir_no].name);
      cpp_pop_buffer (pfile);
    }
}

/* Checks for validity the macro name in #define, #undef, #ifdef and
   #ifndef directives.  */
static cpp_hashnode *
lex_macro_node (pfile)
     cpp_reader *pfile;
{
  cpp_token token;

  /* Lex the macro name directly.  */
  _cpp_lex_token (pfile, &token);

  /* The token immediately after #define must be an identifier.  That
     identifier is not allowed to be "defined".  See predefined macro
     names (6.10.8.4).  In C++, it is not allowed to be any of the
     <iso646.h> macro names (which are keywords in C++) either.  */

  if (token.type != CPP_NAME)
    {
      if (token.type == CPP_EOF)
	cpp_error (pfile, "no macro name given in #%s directive",
		   pfile->directive->name);
      else if (token.flags & NAMED_OP)
	cpp_error (pfile,
		   "\"%s\" cannot be used as a macro name as it is an operator in C++",
		   token.val.node->name);
      else
	cpp_error (pfile, "macro names must be identifiers");
    }
  else
    {
      cpp_hashnode *node = token.val.node;

      /* In Objective C, some keywords begin with '@', but general
	 identifiers do not, and you're not allowed to #define them.  */
      if (node == pfile->spec_nodes.n_defined || node->name[0] == '@')
	cpp_error (pfile, "\"%s\" cannot be used as a macro name", node->name);
      else if (!(node->flags & NODE_POISONED))
	return node;
    }

  return 0;
}

/* Process a #define directive.  Most work is done in cppmacro.c.  */
static void
do_define (pfile)
     cpp_reader *pfile;
{
  cpp_hashnode *node = lex_macro_node (pfile);

  if (node)
    {
      /* Use the permanent pool for storage.  */
      pfile->string_pool = &pfile->ident_pool;

      if (_cpp_create_definition (pfile, node))
	if (pfile->cb.define)
	  (*pfile->cb.define) (pfile, node);

      /* Revert to the temporary pool.  */
      pfile->string_pool = &pfile->temp_string_pool;
    }
}

/* Handle #undef.  Marks the identifier NT_VOID in the hash table.  */
static void
do_undef (pfile)
     cpp_reader *pfile;
{
  cpp_hashnode *node = lex_macro_node (pfile);  

  /* 6.10.3.5 paragraph 2: [#undef] is ignored if the specified identifier
     is not currently defined as a macro name.  */
  if (node && node->type == NT_MACRO)
    {
      if (pfile->cb.undef)
	(*pfile->cb.undef) (pfile, node);

      if (node->flags & NODE_BUILTIN)
	cpp_warning (pfile, "undefining \"%s\"", node->name);

      _cpp_free_definition (node);
    }
  check_eol (pfile);
}

/* Helper routine used by parse_include.  Reinterpret the current line
   as an h-char-sequence (< ... >); we are looking at the first token
   after the <.  Returns zero on success.  */
static int
glue_header_name (pfile, header)
     cpp_reader *pfile;
     cpp_token *header;
{
  cpp_token token;
  unsigned char *buffer, *token_mem;
  size_t len, total_len = 0, capacity = 1024;

  /* To avoid lexed tokens overwriting our glued name, we can only
     allocate from the string pool once we've lexed everything.  */

  buffer = (unsigned char *) xmalloc (capacity);
  for (;;)
    {
      _cpp_get_token (pfile, &token);

      if (token.type == CPP_GREATER || token.type == CPP_EOF)
	break;

      len = cpp_token_len (&token);
      if (total_len + len > capacity)
	{
	  capacity = (capacity + len) * 2;
	  buffer = (unsigned char *) realloc (buffer, capacity);
	}

      if (token.flags & PREV_WHITE)
	buffer[total_len++] = ' ';

      total_len = cpp_spell_token (pfile, &token, &buffer[total_len]) - buffer;
    }

  if (token.type == CPP_EOF)
    cpp_error (pfile, "missing terminating > character");
  else
    {
      token_mem = _cpp_pool_alloc (pfile->string_pool, total_len);
      memcpy (token_mem, buffer, total_len);

      header->type = CPP_HEADER_NAME;
      header->flags &= ~PREV_WHITE;
      header->val.str.len = total_len;
      header->val.str.text = token_mem;
    }

  free ((PTR) buffer);
  return token.type == CPP_EOF;
}

/* Parse the header name of #include, #include_next, #import and
   #pragma dependency.  Returns zero on success.  */
static int
parse_include (pfile, header)
     cpp_reader *pfile;
     cpp_token *header;
{
  int is_pragma = pfile->directive == &dtable[T_PRAGMA];
  const unsigned char *dir;

  if (is_pragma)
    dir = U"pragma dependency";
  else
    dir = pfile->directive->name;

  /* Allow macro expansion.  */
  cpp_get_token (pfile, header);
  if (header->type != CPP_STRING && header->type != CPP_HEADER_NAME)
    {
      if (header->type != CPP_LESS)
	{
	  cpp_error (pfile, "#%s expects \"FILENAME\" or <FILENAME>", dir);
	  return 1;
	}
      if (glue_header_name (pfile, header))
	return 1;
    }

  if (header->val.str.len == 0)
    {
      cpp_error (pfile, "empty file name in #%s", dir);
      return 1;
    }

  if (!is_pragma)
    {
      check_eol (pfile);
      /* Get out of macro context, if we are.  */
      skip_rest_of_line (pfile);
      if (pfile->cb.include)
	(*pfile->cb.include) (pfile, dir, header);
    }

  return 0;
}

static void
do_include (pfile)
     cpp_reader *pfile;
{
  cpp_token header;

  if (!parse_include (pfile, &header))
    _cpp_execute_include (pfile, &header, 0, 0);
}

static void
do_import (pfile)
     cpp_reader *pfile;
{
  cpp_token header;

  if (!pfile->import_warning && CPP_OPTION (pfile, warn_import))
    {
      pfile->import_warning = 1;
      cpp_warning (pfile,
	   "#import is obsolete, use an #ifndef wrapper in the header file");
    }

  if (!parse_include (pfile, &header))
    _cpp_execute_include (pfile, &header, 1, 0);
}

static void
do_include_next (pfile)
     cpp_reader *pfile;
{
  cpp_token header;
  struct file_name_list *search_start = 0;

  if (parse_include (pfile, &header))
    return;

  /* For #include_next, skip in the search path past the dir in which
     the current file was found.  If this is the last directory in the
     search path, don't include anything.  If the current file was
     specified with an absolute path, use the normal search logic.  If
     this is the primary source file, use the normal search logic and
     generate a warning.  */
  if (CPP_PREV_BUFFER (CPP_BUFFER (pfile)))
    {
      if (CPP_BUFFER (pfile)->inc->foundhere)
	{
	  search_start = CPP_BUFFER (pfile)->inc->foundhere->next;
	  if (!search_start)
	    return;
	}
    }
  else
    cpp_warning (pfile, "#include_next in primary source file");

  _cpp_execute_include (pfile, &header, 0, search_start);
}

/* Subroutine of do_line.  Read next token from PFILE without adding it to
   the output buffer.  If it is a number between 1 and 4, store it in *NUM
   and return 1; otherwise, return 0 and complain if we aren't at the end
   of the directive.  */

static int
read_line_number (pfile, num)
     cpp_reader *pfile;
     int *num;
{
  cpp_token token;
  unsigned int val;

  _cpp_lex_token (pfile, &token);
  if (token.type == CPP_NUMBER && token.val.str.len == 1)
    {
      val = token.val.str.text[0] - '1';
      if (val <= 3)
	{
	  *num = val + 1;
	  return 1;
	}
    }

  if (token.type != CPP_EOF)
    cpp_error (pfile, "invalid format #line");
  return 0;
}

/* Another subroutine of do_line.  Convert a number in STR, of length
   LEN, to binary; store it in NUMP, and return 0 if the number was
   well-formed, 1 if not.  Temporary, hopefully.  */
static int
strtoul_for_line (str, len, nump)
     const U_CHAR *str;
     unsigned int len;
     unsigned long *nump;
{
  unsigned long reg = 0;
  U_CHAR c;
  while (len--)
    {
      c = *str++;
      if (!ISDIGIT (c))
	return 1;
      reg *= 10;
      reg += c - '0';
    }
  *nump = reg;
  return 0;
}

/* Interpret #line command.
   Note that the filename string (if any) is treated as if it were an
   include filename.  That means no escape handling.  */

static void
do_line (pfile)
     cpp_reader *pfile;
{
  cpp_buffer *ip = CPP_BUFFER (pfile);
  unsigned long new_lineno;
  /* C99 raised the minimum limit on #line numbers.  */
  unsigned int cap = CPP_OPTION (pfile, c99) ? 2147483647 : 32767;
  int enter = 0, leave = 0, rename = 0;
  cpp_token token;

  /* #line commands expand macros.  */
  _cpp_get_token (pfile, &token);
  if (token.type != CPP_NUMBER
      || strtoul_for_line (token.val.str.text, token.val.str.len, &new_lineno))
    {
      cpp_error (pfile, "\"%s\" after #line is not a positive integer",
		 cpp_token_as_text (pfile, &token));
      return;
    }      

  if (CPP_PEDANTIC (pfile) && (new_lineno == 0 || new_lineno > cap))
    cpp_pedwarn (pfile, "line number out of range");

  _cpp_get_token (pfile, &token);

  if (token.type != CPP_EOF)
    {
      char *fname;
      unsigned int len;
      int action_number = 0;

      if (token.type != CPP_STRING)
	{
	  cpp_error (pfile, "\"%s\" is not a valid filename",
		     cpp_token_as_text (pfile, &token));
	  return;
	}

      len = token.val.str.len;
      fname = alloca (len + 1);
      memcpy (fname, token.val.str.text, len);
      fname[len] = '\0';
    
      if (strcmp (fname, ip->nominal_fname))
	{
	  rename = 1;
	  if (!strcmp (fname, ip->inc->name))
	    ip->nominal_fname = ip->inc->name;
	  else
	    ip->nominal_fname = _cpp_fake_include (pfile, fname);
	}

      if (read_line_number (pfile, &action_number) != 0)
	{
	  if (CPP_PEDANTIC (pfile))
	    cpp_pedwarn (pfile,  "extra tokens at end of #line directive");

	  if (action_number == 1)
	    {
	      enter = 1;
	      cpp_make_system_header (pfile, ip, 0);
	      read_line_number (pfile, &action_number);
	    }
	  else if (action_number == 2)
	    {
	      leave = 1;
	      cpp_make_system_header (pfile, ip, 0);
	      read_line_number (pfile, &action_number);
	    }
	  if (action_number == 3)
	    {
	      cpp_make_system_header (pfile, ip, 1);
	      read_line_number (pfile, &action_number);
	    }
	  if (action_number == 4)
	    {
	      cpp_make_system_header (pfile, ip, 2);
	      read_line_number (pfile, &action_number);
	    }
	}
      check_eol (pfile);
    }

  /* Our line number is incremented after the directive is processed.  */
  ip->lineno = new_lineno - 1;
  pfile->lexer_pos.output_line = ip->lineno;
  if (enter && pfile->cb.enter_file)
    (*pfile->cb.enter_file) (pfile);
  if (leave && pfile->cb.leave_file)
    (*pfile->cb.leave_file) (pfile);
  if (rename && pfile->cb.rename_file)
    (*pfile->cb.rename_file) (pfile);
}

/*
 * Report a warning or error detected by the program we are
 * processing.  Use the directive's tokens in the error message.
 */

static void
do_diagnostic (pfile, code)
     cpp_reader *pfile;
     enum error_type code;
{
  if (_cpp_begin_message (pfile, code, NULL, 0))
    {
      fprintf (stderr, "#%s ", pfile->directive->name);
      pfile->state.prevent_expansion++;
      cpp_output_line (pfile, stderr);
      pfile->state.prevent_expansion--;
    }
}

static void
do_error (pfile)
     cpp_reader *pfile;
{
  do_diagnostic (pfile, ERROR);
}

static void
do_warning (pfile)
     cpp_reader *pfile;
{
  do_diagnostic (pfile, WARNING);
}

/* Report program identification.  */

static void
do_ident (pfile)
     cpp_reader *pfile;
{
  cpp_token str;

  _cpp_get_token (pfile, &str);
  if (str.type != CPP_STRING)
    cpp_error (pfile, "invalid #ident");
  else if (pfile->cb.ident)
    (*pfile->cb.ident) (pfile, &str.val.str);

  check_eol (pfile);
}

/* Pragmata handling.  We handle some of these, and pass the rest on
   to the front end.  C99 defines three pragmas and says that no macro
   expansion is to be performed on them; whether or not macro
   expansion happens for other pragmas is implementation defined.
   This implementation never macro-expands the text after #pragma.

   We currently do not support the _Pragma operator.  Support for that
   has to be coordinated with the front end.  Proposed implementation:
   both #pragma blah blah and _Pragma("blah blah") become
   __builtin_pragma(blah blah) and we teach the parser about that.  */

/* Sub-handlers for the pragmas needing treatment here.
   They return 1 if the token buffer is to be popped, 0 if not. */
struct pragma_entry
{
  struct pragma_entry *next;
  const char *name;
  size_t len;
  int isnspace;
  union {
    void (*handler) PARAMS ((cpp_reader *));
    struct pragma_entry *space;
  } u;
};

void
cpp_register_pragma (pfile, space, name, handler)
     cpp_reader *pfile;
     const char *space;
     const char *name;
     void (*handler) PARAMS ((cpp_reader *));
{
  struct pragma_entry **x, *new;
  size_t len;

  x = &pfile->pragmas;
  if (space)
    {
      struct pragma_entry *p = pfile->pragmas;
      len = strlen (space);
      while (p)
	{
	  if (p->isnspace && p->len == len && !memcmp (p->name, space, len))
	    {
	      x = &p->u.space;
	      goto found;
	    }
	  p = p->next;
	}
      cpp_ice (pfile, "unknown #pragma namespace %s", space);
      return;
    }

 found:
  new = xnew (struct pragma_entry);
  new->name = name;
  new->len = strlen (name);
  new->isnspace = 0;
  new->u.handler = handler;

  new->next = *x;
  *x = new;
}

void
cpp_register_pragma_space (pfile, space)
     cpp_reader *pfile;
     const char *space;
{
  struct pragma_entry *new;
  const struct pragma_entry *p = pfile->pragmas;
  size_t len = strlen (space);

  while (p)
    {
      if (p->isnspace && p->len == len && !memcmp (p->name, space, len))
	/* Multiple different callers are allowed to register the same
	   namespace.  */
	return;
      p = p->next;
    }

  new = xnew (struct pragma_entry);
  new->name = space;
  new->len = len;
  new->isnspace = 1;
  new->u.space = 0;

  new->next = pfile->pragmas;
  pfile->pragmas = new;
}
  
void
_cpp_init_internal_pragmas (pfile)
     cpp_reader *pfile;
{
  /* top level */
  cpp_register_pragma (pfile, 0, "poison", do_pragma_poison);
  cpp_register_pragma (pfile, 0, "once", do_pragma_once);

  /* GCC namespace */
  cpp_register_pragma_space (pfile, "GCC");

  cpp_register_pragma (pfile, "GCC", "poison", do_pragma_poison);
  cpp_register_pragma (pfile, "GCC", "system_header", do_pragma_system_header);
  cpp_register_pragma (pfile, "GCC", "dependency", do_pragma_dependency);
}

static void
do_pragma (pfile)
     cpp_reader *pfile;
{
  const struct pragma_entry *p;
  cpp_token tok;
  const cpp_hashnode *node;
  const U_CHAR *name;
  size_t len;
  int drop = 0;

  p = pfile->pragmas;
  pfile->state.prevent_expansion++;
  cpp_start_lookahead (pfile);

 new_space:
  cpp_get_token (pfile, &tok);
  if (tok.type == CPP_NAME)
    {
      node = tok.val.node;
      name = node->name;
      len = node->length;
      while (p)
	{
	  if (strlen (p->name) == len && !memcmp (p->name, name, len))
	    {
	      if (p->isnspace)
		{
		  p = p->u.space;
		  goto new_space;
		}
	      else
		{
		  (*p->u.handler) (pfile);
		  drop = 1;
		  break;
		}
	    }
	  p = p->next;
	}
    }

  cpp_stop_lookahead (pfile, drop);
  pfile->state.prevent_expansion--;

  if (!drop && pfile->cb.def_pragma)
    (*pfile->cb.def_pragma) (pfile);
}

static void
do_pragma_once (pfile)
     cpp_reader *pfile;
{
  cpp_buffer *ip = CPP_BUFFER (pfile);

  cpp_warning (pfile, "#pragma once is obsolete");
 
  if (CPP_PREV_BUFFER (ip) == NULL)
    cpp_warning (pfile, "#pragma once in main file");
  else
    ip->inc->cmacro = NEVER_REREAD;

  check_eol (pfile);
}

static void
do_pragma_poison (pfile)
     cpp_reader *pfile;
{
  /* Poison these symbols so that all subsequent usage produces an
     error message.  */
  cpp_token tok;
  cpp_hashnode *hp;

  pfile->state.poisoned_ok = 1;
  for (;;)
    {
      _cpp_lex_token (pfile, &tok);
      if (tok.type == CPP_EOF)
	break;
      if (tok.type != CPP_NAME)
	{
	  cpp_error (pfile, "invalid #pragma GCC poison directive");
	  break;
	}

      hp = tok.val.node;
      if (hp->flags & NODE_POISONED)
	continue;

      if (hp->type == NT_MACRO)
	cpp_warning (pfile, "poisoning existing macro \"%s\"", hp->name);
      _cpp_free_definition (hp);
      hp->flags |= NODE_POISONED | NODE_DIAGNOSTIC;
    }
  pfile->state.poisoned_ok = 0;

#if 0				/* Doesn't quite work yet.  */
  if (tok.type == CPP_EOF && pfile->cb.poison)
    (*pfile->cb.poison) (pfile);
#endif
}

/* Mark the current header as a system header.  This will suppress
   some categories of warnings (notably those from -pedantic).  It is
   intended for use in system libraries that cannot be implemented in
   conforming C, but cannot be certain that their headers appear in a
   system include directory.  To prevent abuse, it is rejected in the
   primary source file.  */
static void
do_pragma_system_header (pfile)
     cpp_reader *pfile;
{
  cpp_buffer *ip = CPP_BUFFER (pfile);
  if (CPP_PREV_BUFFER (ip) == NULL)
    cpp_warning (pfile, "#pragma system_header outside include file");
  else
    cpp_make_system_header (pfile, ip, 1);

  check_eol (pfile);
}

/* Check the modified date of the current include file against a specified
   file. Issue a diagnostic, if the specified file is newer. We use this to
   determine if a fixed header should be refixed.  */
static void
do_pragma_dependency (pfile)
     cpp_reader *pfile;
{
  cpp_token header, msg;
  int ordering;
 
  if (parse_include (pfile, &header))
    return;

  ordering = _cpp_compare_file_date (pfile, &header);
  if (ordering < 0)
    cpp_warning (pfile, "cannot find source %s",
		 cpp_token_as_text (pfile, &header));
  else if (ordering > 0)
    {
      cpp_warning (pfile, "current file is older than %s",
		   cpp_token_as_text (pfile, &header));
      cpp_start_lookahead (pfile);
      cpp_get_token (pfile, &msg);
      cpp_stop_lookahead (pfile, msg.type == CPP_EOF);
      if (msg.type != CPP_EOF && _cpp_begin_message (pfile, WARNING, NULL, 0))
	cpp_output_line (pfile, stderr);
    }
}

/* Check syntax is "(string-literal)".  Returns 0 on success.  */
static int
get__Pragma_string (pfile, string)
     cpp_reader *pfile;
     cpp_token *string;
{
  cpp_token paren;

  cpp_get_token (pfile, &paren);
  if (paren.type != CPP_OPEN_PAREN)
    return 1;

  cpp_get_token (pfile, string);
  if (string->type != CPP_STRING && string->type != CPP_WSTRING)
    return 1;

  cpp_get_token (pfile, &paren);
  return paren.type != CPP_CLOSE_PAREN;
}

/* Returns a malloced buffer containing a destringized cpp_string by
   removing the first \ of \" and \\ sequences.  */
static unsigned char *
destringize (in, len)
     const cpp_string *in;
     unsigned int *len;
{
  const unsigned char *src, *limit;
  unsigned char *dest, *result;

  dest = result = (unsigned char *) xmalloc (in->len);
  for (src = in->text, limit = src + in->len; src < limit;)
    {
      /* We know there is a character following the backslash.  */
      if (*src == '\\' && (src[1] == '\\' || src[1] == '"'))
	src++;
      *dest++ = *src++;
    }

  *len = dest - result;
  return result;
}

void
_cpp_do__Pragma (pfile)
     cpp_reader *pfile;
{
  cpp_token string;
  unsigned char *buffer;
  unsigned int len;

  if (get__Pragma_string (pfile, &string))
    {
      cpp_error (pfile, "_Pragma takes a parenthesized string literal");
      return;
    }

  buffer = destringize (&string.val.str, &len);
  run_directive (pfile, T_PRAGMA, (char *) buffer, len, _("<_Pragma>"));
  free ((PTR) buffer);
}

/* Just ignore #sccs, on systems where we define it at all.  */
#ifdef SCCS_DIRECTIVE
static void
do_sccs (pfile)
     cpp_reader *pfile ATTRIBUTE_UNUSED;
{
}
#endif

static void
do_ifdef (pfile)
     cpp_reader *pfile;
{
  int skip = 1;

  if (! pfile->skipping)
    {
      const cpp_hashnode *node = lex_macro_node (pfile);

      if (node)
	skip = node->type != NT_MACRO;
    }

  push_conditional (pfile, skip, T_IFDEF, 0);
}

static void
do_ifndef (pfile)
     cpp_reader *pfile;
{
  int skip = 1;
  const cpp_hashnode *node = 0;

  if (! pfile->skipping)
    {
      node = lex_macro_node (pfile);
      if (node)
	skip = node->type == NT_MACRO;
    }

  push_conditional (pfile, skip, T_IFNDEF, node);
}

/* #if cooperates with parse_defined to handle multiple-include
   optimisations.  If macro expansions or identifiers appear in the
   expression, we cannot treat it as a controlling conditional, since
   their values could change in the future.  */

static void
do_if (pfile)
     cpp_reader *pfile;
{
  int skip = 1;
  const cpp_hashnode *cmacro = 0;

  if (!pfile->skipping)
    {
      /* Controlling macro of #if ! defined ()  */
      pfile->mi_ind_cmacro = 0;
      skip = _cpp_parse_expr (pfile) == 0;
      cmacro = pfile->mi_ind_cmacro;
    }

  push_conditional (pfile, skip, T_IF, cmacro);
}

/* #else flips pfile->skipping and continues without changing
   if_stack; this is so that the error message for missing #endif's
   etc. will point to the original #if.  */

static void
do_else (pfile)
     cpp_reader *pfile;
{
  struct if_stack *ifs = CPP_BUFFER (pfile)->if_stack;

  if (ifs == NULL)
    cpp_error (pfile, "#else without #if");
  else
    {
      if (ifs->type == T_ELSE)
	{
	  cpp_error (pfile, "#else after #else");
	  cpp_error_with_line (pfile, ifs->pos.line, ifs->pos.col,
			       "the conditional began here");
	}

      /* Invalidate any controlling macro.  */
      ifs->mi_cmacro = 0;

      ifs->type = T_ELSE;
      if (! ifs->was_skipping)
	{
	  /* If pfile->skipping is 2, one of the blocks in an #if
	     #elif ... chain succeeded, so we skip the else block.  */
	  if (pfile->skipping < 2)
	    pfile->skipping = ! pfile->skipping;
	}
    }

  check_eol (pfile);
}

/* handle a #elif directive by not changing if_stack either.  see the
   comment above do_else.  */

static void
do_elif (pfile)
     cpp_reader *pfile;
{
  struct if_stack *ifs = CPP_BUFFER (pfile)->if_stack;

  if (ifs == NULL)
    {
      cpp_error (pfile, "#elif without #if");
      return;
    }

  if (ifs->type == T_ELSE)
    {
      cpp_error (pfile, "#elif after #else");
      cpp_error_with_line (pfile, ifs->pos.line, ifs->pos.col,
			   "the conditional began here");
    }

  /* Invalidate any controlling macro.  */
  ifs->mi_cmacro = 0;

  ifs->type = T_ELIF;
  if (ifs->was_skipping)
    return;  /* Don't evaluate a nested #if */

  if (pfile->skipping != 1)
    {
      pfile->skipping = 2;  /* one block succeeded, so don't do any others */
      return;
    }

  pfile->skipping = ! _cpp_parse_expr (pfile);
}

/* #endif pops the if stack and resets pfile->skipping.  */

static void
do_endif (pfile)
     cpp_reader *pfile;
{
  struct if_stack *ifs = CPP_BUFFER (pfile)->if_stack;

  if (ifs == NULL)
    cpp_error (pfile, "#endif without #if");
  else
    {
      /* If potential control macro, we go back outside again.  */
      if (ifs->next == 0 && ifs->mi_cmacro)
	{
	  pfile->mi_state = MI_OUTSIDE;
	  pfile->mi_cmacro = ifs->mi_cmacro;
	}

      CPP_BUFFER (pfile)->if_stack = ifs->next;
      pfile->skipping = ifs->was_skipping;
      obstack_free (pfile->buffer_ob, ifs);
    }

  check_eol (pfile);
}

/* Push an if_stack entry and set pfile->skipping accordingly.
   If this is a #ifndef starting at the beginning of a file,
   CMACRO is the macro name tested by the #ifndef.  */

static void
push_conditional (pfile, skip, type, cmacro)
     cpp_reader *pfile;
     int skip;
     int type;
     const cpp_hashnode *cmacro;
{
  struct if_stack *ifs;

  ifs = xobnew (pfile->buffer_ob, struct if_stack);
  ifs->pos = pfile->directive_pos;
  ifs->next = CPP_BUFFER (pfile)->if_stack;
  ifs->was_skipping = pfile->skipping;
  ifs->type = type;
  if (pfile->mi_state == MI_OUTSIDE && pfile->mi_cmacro == 0)
    ifs->mi_cmacro = cmacro;
  else
    ifs->mi_cmacro = 0;

  if (!pfile->skipping)
    pfile->skipping = skip;

  CPP_BUFFER (pfile)->if_stack = ifs;
}

/* Called when we reach the end of a file.  Walk back up the
   conditional stack till we reach its level at entry to this file,
   issuing error messages.  Then force skipping off.  */
static void
unwind_if_stack (pfile, pbuf)
     cpp_reader *pfile;
     cpp_buffer *pbuf;
{
  struct if_stack *ifs;

  /* No need to free stack - they'll all go away with the buffer.  */
  for (ifs = pbuf->if_stack; ifs; ifs = ifs->next)
    cpp_error_with_line (pfile, ifs->pos.line, ifs->pos.col,
			 "unterminated #%s", dtable[ifs->type].name);

  pfile->skipping = 0;
}

/* Read the tokens of the answer into the macro pool.  Only commit the
   memory if we intend it as permanent storage, i.e. the #assert case.
   Returns 0 on success.  */

static int
parse_answer (pfile, answerp, type)
     cpp_reader *pfile;
     struct answer **answerp;
     int type;
{
  cpp_token paren, *token;
  struct answer *answer;

  if (POOL_FRONT (&pfile->macro_pool) + sizeof (struct answer) >
      POOL_LIMIT (&pfile->macro_pool))
    _cpp_next_chunk (&pfile->macro_pool, sizeof (struct answer), 0);
  answer = (struct answer *) POOL_FRONT (&pfile->macro_pool);
  answer->count = 0;

  /* In a conditional, it is legal to not have an open paren.  We
     should save the following token in this case.  */
  if (type == T_IF)
    cpp_start_lookahead (pfile);
  cpp_get_token (pfile, &paren);
  if (type == T_IF)
    cpp_stop_lookahead (pfile, paren.type == CPP_OPEN_PAREN);

  /* If not a paren, see if we're OK.  */
  if (paren.type != CPP_OPEN_PAREN)
    {
      /* In a conditional no answer is a test for any answer.  It
         could be followed by any token.  */
      if (type == T_IF)
	return 0;

      /* #unassert with no answer is valid - it removes all answers.  */
      if (type == T_UNASSERT && paren.type == CPP_EOF)
	return 0;

      cpp_error (pfile, "missing '(' after predicate");
      return 1;
    }

  for (;;)
    {
      token = &answer->first[answer->count];
      /* Check we have room for the token.  */
      if ((unsigned char *) (token + 1) >= POOL_LIMIT (&pfile->macro_pool))
	{
	  _cpp_next_chunk (&pfile->macro_pool, sizeof (cpp_token),
			   (unsigned char **) &answer);
	  token = &answer->first[answer->count];
	}

      _cpp_get_token (pfile, token);
      if (token->type == CPP_CLOSE_PAREN)
	break;

      if (token->type == CPP_EOF)
	{
	  cpp_error (pfile, "missing ')' to complete answer");
	  return 1;
	}
      answer->count++;
    }

  if (answer->count == 0)
    {
      cpp_error (pfile, "predicate's answer is empty");
      return 1;
    }

  /* Drop whitespace at start.  */
  answer->first->flags &= ~PREV_WHITE;
  *answerp = answer;

  if (type == T_ASSERT || type == T_UNASSERT)
    check_eol (pfile);
  return 0;
}

/* Parses an assertion, returning a pointer to the hash node of the
   predicate, or 0 on error.  If an answer was supplied, it is placed
   in ANSWERP, otherwise it is set to 0.  We use _cpp_get_raw_token,
   since we cannot assume tokens are consecutive in a #if statement
   (we may be in a macro), and we don't want to macro expand.  */
static cpp_hashnode *
parse_assertion (pfile, answerp, type)
     cpp_reader *pfile;
     struct answer **answerp;
     int type;
{
  cpp_hashnode *result = 0;
  cpp_token predicate;

  /* We don't expand predicates or answers.  */
  pfile->state.prevent_expansion++;

  /* Use the permanent pool for storage (for the answers).  */
  pfile->string_pool = &pfile->ident_pool;

  *answerp = 0;
  _cpp_get_token (pfile, &predicate);
  if (predicate.type == CPP_EOF)
    cpp_error (pfile, "assertion without predicate");
  else if (predicate.type != CPP_NAME)
    cpp_error (pfile, "predicate must be an identifier");
  else if (parse_answer (pfile, answerp, type) == 0)
    {
      unsigned int len = predicate.val.node->length;
      unsigned char *sym = alloca (len + 1);

      /* Prefix '#' to get it out of macro namespace.  */
      sym[0] = '#';
      memcpy (sym + 1, predicate.val.node->name, len);
      result = cpp_lookup (pfile, sym, len + 1);
    }

  pfile->string_pool = &pfile->temp_string_pool;
  pfile->state.prevent_expansion--;
  return result;
}

/* Returns a pointer to the pointer to the answer in the answer chain,
   or a pointer to NULL if the answer is not in the chain.  */
static struct answer **
find_answer (node, candidate)
     cpp_hashnode *node;
     const struct answer *candidate;
{
  unsigned int i;
  struct answer **result;

  for (result = &node->value.answers; *result; result = &(*result)->next)
    {
      struct answer *answer = *result;

      if (answer->count == candidate->count)
	{
	  for (i = 0; i < answer->count; i++)
	    if (! _cpp_equiv_tokens (&answer->first[i], &candidate->first[i]))
	      break;

	  if (i == answer->count)
	    break;
	}
    }

  return result;
}

/* Test an assertion within a preprocessor conditional.  Returns
   non-zero on failure, zero on success.  On success, the result of
   the test is written into VALUE.  */
int
_cpp_test_assertion (pfile, value)
     cpp_reader *pfile;
     int *value;
{
  struct answer *answer;
  cpp_hashnode *node;

  node = parse_assertion (pfile, &answer, T_IF);
  if (node)
    *value = (node->type == NT_ASSERTION &&
	      (answer == 0 || *find_answer (node, answer) != 0));

  /* We don't commit the memory for the answer - it's temporary only.  */
  return node == 0;
}

static void
do_assert (pfile)
     cpp_reader *pfile;
{
  struct answer *new_answer;
  cpp_hashnode *node;
  
  node = parse_assertion (pfile, &new_answer, T_ASSERT);
  if (node)
    {
      /* Place the new answer in the answer list.  First check there
         is not a duplicate.  */
      new_answer->next = 0;
      if (node->type == NT_ASSERTION)
	{
	  if (*find_answer (node, new_answer))
	    {
	      cpp_warning (pfile, "\"%s\" re-asserted", node->name + 1);
	      return;
	    }
	  new_answer->next = node->value.answers;
	}
      node->type = NT_ASSERTION;
      node->value.answers = new_answer;
      POOL_COMMIT (&pfile->macro_pool, (sizeof (struct answer)
					+ (new_answer->count - 1)
					* sizeof (cpp_token)));
    }
}

static void
do_unassert (pfile)
     cpp_reader *pfile;
{
  cpp_hashnode *node;
  struct answer *answer;
  
  node = parse_assertion (pfile, &answer, T_UNASSERT);
  /* It isn't an error to #unassert something that isn't asserted.  */
  if (node && node->type == NT_ASSERTION)
    {
      if (answer)
	{
	  struct answer **p = find_answer (node, answer), *temp;

	  /* Remove the answer from the list.  */
	  temp = *p;
	  if (temp)
	    *p = temp->next;

	  /* Did we free the last answer?  */
	  if (node->value.answers == 0)
	    node->type = NT_VOID;
	}
      else
	_cpp_free_definition (node);
    }

  /* We don't commit the memory for the answer - it's temporary only.  */
}

/* These are for -D, -U, -A.  */

/* Process the string STR as if it appeared as the body of a #define.
   If STR is just an identifier, define it with value 1.
   If STR has anything after the identifier, then it should
   be identifier=definition. */

void
cpp_define (pfile, str)
     cpp_reader *pfile;
     const char *str;
{
  char *buf, *p;
  size_t count;

  /* Copy the entire option so we can modify it. 
     Change the first "=" in the string to a space.  If there is none,
     tack " 1" on the end.  */

  /* Length including the null.  */  
  count = strlen (str);
  buf = (char *) alloca (count + 2);
  memcpy (buf, str, count);

  p = strchr (str, '=');
  if (p)
    buf[p - str] = ' ';
  else
    {
      buf[count++] = ' ';
      buf[count++] = '1';
    }

  run_directive (pfile, T_DEFINE, buf, count, 0);
}

/* Slight variant of the above for use by initialize_builtins, which (a)
   knows how to set up the buffer itself, (b) needs a different "filename"
   tag.  */
void
_cpp_define_builtin (pfile, str)
     cpp_reader *pfile;
     const char *str;
{
  run_directive (pfile, T_DEFINE, str, strlen (str), _("<builtin>"));
}

/* Process MACRO as if it appeared as the body of an #undef.  */
void
cpp_undef (pfile, macro)
     cpp_reader *pfile;
     const char *macro;
{
  run_directive (pfile, T_UNDEF, macro, strlen (macro), 0);
}

/* Process the string STR as if it appeared as the body of a #assert. */
void
cpp_assert (pfile, str)
     cpp_reader *pfile;
     const char *str;
{
  handle_assertion (pfile, str, T_ASSERT);
}

/* Process STR as if it appeared as the body of an #unassert. */
void
cpp_unassert (pfile, str)
     cpp_reader *pfile;
     const char *str;
{
  handle_assertion (pfile, str, T_UNASSERT);
}  

/* Common code for cpp_assert (-A) and cpp_unassert (-A-).  */
static void
handle_assertion (pfile, str, type)
     cpp_reader *pfile;
     const char *str;
     int type;
{
  size_t count = strlen (str);
  const char *p = strchr (str, '=');

  if (p)
    {
      /* Copy the entire option so we can modify it.  Change the first
	 "=" in the string to a '(', and tack a ')' on the end.  */
      char *buf = (char *) alloca (count + 1);

      memcpy (buf, str, count);
      buf[p - str] = '(';
      buf[count++] = ')';
      str = buf;
    }

  run_directive (pfile, type, str, count, 0);
}

/* Determine whether the identifier ID, of length LEN, is a defined macro.  */
int
cpp_defined (pfile, id, len)
     cpp_reader *pfile;
     const U_CHAR *id;
     int len;
{
  cpp_hashnode *hp = cpp_lookup (pfile, id, len);

  /* If it's of type NT_MACRO, it cannot be poisoned.  */
  return hp->type == NT_MACRO;
}

/* Allocate a new cpp_buffer for PFILE, and push it on the input
   buffer stack.  If BUFFER != NULL, then use the LENGTH characters in
   BUFFER as the new input buffer.  Return the new buffer, or NULL on
   failure.  */

cpp_buffer *
cpp_push_buffer (pfile, buffer, length)
     cpp_reader *pfile;
     const U_CHAR *buffer;
     long length;
{
  cpp_buffer *buf = CPP_BUFFER (pfile);
  cpp_buffer *new;
  if (++pfile->buffer_stack_depth == CPP_STACK_MAX)
    {
      cpp_fatal (pfile, "#include nested too deeply");
      return NULL;
    }

  new = xobnew (pfile->buffer_ob, cpp_buffer);
  /* Clears, amongst other things, if_stack and mi_cmacro.  */
  memset (new, 0, sizeof (cpp_buffer));

  pfile->lexer_pos.output_line = 1;
  new->line_base = new->buf = new->cur = buffer;
  new->rlimit = buffer + length;
  new->prev = buf;
  new->pfile = pfile;
  /* Preprocessed files don't do trigraph and escaped newline processing.  */
  new->from_stage3 = CPP_OPTION (pfile, preprocessed);
  /* No read ahead or extra char initially.  */
  new->read_ahead = EOF;
  new->extra_char = EOF;
  pfile->state.skip_newlines = 1;

  CPP_BUFFER (pfile) = new;
  return new;
}

cpp_buffer *
cpp_pop_buffer (pfile)
     cpp_reader *pfile;
{
  int wfb;
  cpp_buffer *buf = CPP_BUFFER (pfile);

  unwind_if_stack (pfile, buf);
  wfb = (buf->inc != 0);
  if (wfb)
    _cpp_pop_file_buffer (pfile, buf);

  CPP_BUFFER (pfile) = CPP_PREV_BUFFER (buf);
  obstack_free (pfile->buffer_ob, buf);
  pfile->buffer_stack_depth--;

  if (CPP_BUFFER (pfile) && wfb && pfile->cb.leave_file)
    (*pfile->cb.leave_file) (pfile);
  
  return CPP_BUFFER (pfile);
}

#define obstack_chunk_alloc xmalloc
#define obstack_chunk_free free
void
_cpp_init_stacks (pfile)
     cpp_reader *pfile;
{
  int i;
  cpp_hashnode *node;

  pfile->buffer_ob = xnew (struct obstack);
  obstack_init (pfile->buffer_ob);

  /* Register the directives.  */
  for (i = 1; i < N_DIRECTIVES; i++)
    {
      node = cpp_lookup (pfile, dtable[i - 1].name, dtable[i - 1].length);
      node->directive_index = i;
    }
}

void
_cpp_cleanup_stacks (pfile)
     cpp_reader *pfile;
{
  obstack_free (pfile->buffer_ob, 0);
  free (pfile->buffer_ob);
}
