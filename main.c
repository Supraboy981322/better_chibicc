#include "oskar.h"

StringArray include_paths;
bool opt_fcommon = true;
bool opt_fpic;

Opts opts = {
  .consume_args = 0,
};

static StringArray passed_args;

static StringArray ld_extra_args;
static StringArray std_include_paths;

char *base_file;
static char *output_file;

static StringArray input_paths;
static StringArray tmpfiles;

const char* token_kind_str[] = {
  [TK_IDENT] = "IDENT",
  [TK_PUNCT] = "PUNCT",
  [TK_KEYWORD] = "KEYWORD",
  [TK_STR] = "STR",
  [TK_NUM] = "NUM",
  [TK_PP_NUM] = "PP_NUM",
  [TK_EOF] = "EOF",
};

static void usage(int status) {
  fprintf(stderr, "oskar [ -o <path> ] [ run ] <file>\n");
  exit(status);
}

static bool take_arg(char *arg) {
  char *x[] = {
    "-o", "-I", "-idirafter", "-include", "-x", "-MF", "-MT", "-Xlinker",
  };

  for (size_t i = 0; i < sizeof(x) / sizeof(*x); i++)
    if (!strcmp(arg, x[i]))
      return true;
  return false;
}

static void add_default_include_paths(char *argv0) {
  // We expect that oskar-specific include files are installed
  // to ./include relative to argv[0].
  strarray_push(&include_paths, format("%s/include", dirname(strdup(argv0))));

  // Add standard include paths. (it's not that hard to read $CPATH)
  strarray_push(&include_paths, "/usr/local/include");
  strarray_push(&include_paths, "/usr/include/x86_64-linux-gnu");
  strarray_push(&include_paths, "/usr/include");

  char* cpath = getenv("CPATH");
  if (cpath) {
    int start = 0;
    int p_len = strlen(cpath);
    for (int i = 0; i < p_len; i++) {
      if (cpath[i] == ':') {
        cpath[i] = 0;
        strarray_push(&include_paths, cpath+start);
        start = i+1;
      }
    }
    if (start < p_len)
      strarray_push(&include_paths, cpath+start);
  }

  // Keep a copy of the standard include paths for -MMD option.
  for (int i = 0; i < include_paths.len; i++)
    strarray_push(&std_include_paths, include_paths.data[i]);
}

static void define(char *str) {
  char *eq = strchr(str, '=');
  if (eq)
    define_macro(strndup(str, eq - str), eq + 1);
  else
    define_macro(str, "1");
}

static FileType parse_opt_x(char *s) {
  if (!strcmp(s, "c"))
    return FILE_C;
  if (!strcmp(s, "assembler"))
    return FILE_ASM;
  if (!strcmp(s, "none"))
    return FILE_NONE;
  had_error("<command line>: unknown argument for -x: %s", s);
}

static char *quote_makefile(char *s) {
  char *buf = calloc(1, strlen(s) * 2 + 1);

  for (int i = 0, j = 0; s[i]; i++) {
    switch (s[i]) {
    case '$':
      buf[j++] = '$';
      buf[j++] = '$';
      break;
    case '#':
      buf[j++] = '\\';
      buf[j++] = '#';
      break;
    case ' ':
    case '\t':
      for (int k = i - 1; k >= 0 && s[k] == '\\'; k--)
        buf[j++] = '\\';
      buf[j++] = '\\';
      buf[j++] = s[i];
      break;
    default:
      buf[j++] = s[i];
      break;
    }
  }
  return buf;
}

// TODO: refactor arg parsing; this is kind-of dumb
static void parse_args(int argc, char **argv) {
  // Make sure that all command line options that take an argument
  // have an argument.
  for (int i = 1; i < argc; i++)
    if (take_arg(argv[i]))
      if (!argv[++i])
        usage(1);

  StringArray idirafter = {};

  for (int i = 1; i < argc && !opts.consume_args; i++) {
    if (!strcmp(argv[i], "-###")) {
      opts.hash_hash_hash = true;
      continue;
    }

    if (!strcmp(argv[i], "-cc1")) {
      opts.cc1 = true;
      continue;
    }

    if (!strcmp(argv[i], "--help"))
      usage(0);

    if (!strcmp(argv[i], "run")) {
      opts.run = true;
      continue;
    }

    if (!strcmp(argv[i], "--")) {
      todo("arg '--'", "consume args", abort);
      opts.consume_args = i;
      continue;
    }

    if (!strcmp(argv[i], "-verbose")) {
      opts.verbose = true;
      continue;
    }

    if (!strcmp(argv[i], "-o")) {
      opts.o = argv[++i];
      continue;
    }

    if (!strncmp(argv[i], "-o", 2)) {
      opts.o = argv[i] + 2;
      continue;
    }

    if (!strcmp(argv[i], "-S")) {
      opts.S = true;
      continue;
    }

    if (!strcmp(argv[i], "-fcommon")) {
      opt_fcommon = true;
      continue;
    }

    if (!strcmp(argv[i], "-fno-common")) {
      opt_fcommon = false;
      continue;
    }

    if (!strcmp(argv[i], "-c")) {
      opts.c = true;
      continue;
    }

    if (!strcmp(argv[i], "-E")) {
      opts.E = true;
      continue;
    }

    if (!strncmp(argv[i], "-I", 2)) {
      strarray_push(&include_paths, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-D")) {
      define(argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-D", 2)) {
      define(argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-U")) {
      undef_macro(argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-U", 2)) {
      undef_macro(argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-include")) {
      strarray_push(&opts.include, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-x")) {
      opts.x = parse_opt_x(argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-x", 2)) {
      opts.x = parse_opt_x(argv[i] + 2);
      continue;
    }

    if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-Wl,", 4)) {
      strarray_push(&input_paths, argv[i]);
      continue;
    }

    if (!strcmp(argv[i], "-Xlinker")) {
      strarray_push(&ld_extra_args, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-s")) {
      strarray_push(&ld_extra_args, "-s");
      continue;
    }

    if (!strcmp(argv[i], "-M")) {
      opts.M = true;
      continue;
    }

    if (!strcmp(argv[i], "-MF")) {
      opts.MF = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-MP")) {
      opts.MP = true;
      continue;
    }

    if (!strcmp(argv[i], "-MT")) {
      if (opts.MT == NULL)
        opts.MT = argv[++i];
      else
        opts.MT = format("%s %s", opts.MT, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-MD")) {
      opts.MD = true;
      continue;
    }

    if (!strcmp(argv[i], "-MQ")) {
      if (opts.MT == NULL)
        opts.MT = quote_makefile(argv[++i]);
      else
        opts.MT = format("%s %s", opts.MT, quote_makefile(argv[++i]));
      continue;
    }

    if (!strcmp(argv[i], "-MMD")) {
      opts.MD = opts.MMD = true;
      continue;
    }

    if (!strcmp(argv[i], "-fpic") || !strcmp(argv[i], "-fPIC")) {
      opt_fpic = true;
      continue;
    }

    if (!strcmp(argv[i], "-cc1-input")) {
      base_file = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-cc1-output")) {
      output_file = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-idirafter")) {
      strarray_push(&idirafter, argv[i++]);
      continue;
    }

    if (!strcmp(argv[i], "-static")) {
      opts.linkage = LINK_STATIC;
      strarray_push(&ld_extra_args, "-static");
      continue;
    }

    if (!strcmp(argv[i], "-shared")) {
      opts.linkage = LINK_SHARED;
      strarray_push(&ld_extra_args, "-shared");
      continue;
    }

    if (!strcmp(argv[i], "-L")) {
      strarray_push(&ld_extra_args, "-L");
      strarray_push(&ld_extra_args, argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-L", 2)) {
      strarray_push(&ld_extra_args, "-L");
      strarray_push(&ld_extra_args, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-hashmap-test")) {
      hashmap_test();
      exit(0);
    }

    // These options are ignored for now.
    if (!strncmp(argv[i], "-O", 2) ||
        !strncmp(argv[i], "-W", 2) ||
        !strncmp(argv[i], "-g", 2) ||
        !strncmp(argv[i], "-std=", 5) ||
        !strcmp(argv[i], "-ffreestanding") ||
        !strcmp(argv[i], "-fno-builtin") ||
        !strcmp(argv[i], "-fno-omit-frame-pointer") ||
        !strcmp(argv[i], "-fno-stack-protector") ||
        !strcmp(argv[i], "-fno-strict-aliasing") ||
        !strcmp(argv[i], "-m64") ||
        !strcmp(argv[i], "-mno-red-zone") ||
        !strcmp(argv[i], "-w"))
      continue;

    if (argv[i][0] == '-' && argv[i][1] != '\0')
      had_error("unknown argument: %s", argv[i]);

    strarray_push(&input_paths, argv[i]);
  }

  for (int i = 0; i < idirafter.len; i++)
    strarray_push(&include_paths, idirafter.data[i]);

  if (input_paths.len == 0)
    had_error("no input files");

  // -E implies that the input is the C macro language.
  if (opts.E)
    opts.x = FILE_C;
}

static FILE *open_file(char *path) {
  if (!path || strcmp(path, "-") == 0)
    return stdout;

  FILE *out = fopen(path, "w");
  if (!out)
    had_error("cannot open output file: %s: %s", path, strerror(errno));
  return out;
}

static bool endswith(char *p, char *q) {
  int len1 = strlen(p);
  int len2 = strlen(q);
  return (len1 >= len2) && !strcmp(p + len1 - len2, q);
}

// Replace file extension
static char *replace_extn(char *tmpl, char *extn) {
  char *filename = basename(strdup(tmpl));
  char *dot = strrchr(filename, '.');
  if (dot)
    *dot = '\0';
  return format("%s%s", filename, extn);
}

static void cleanup(void) {
  for (int i = 0; i < tmpfiles.len; i++)
    unlink(tmpfiles.data[i]);
}

static char *create_tmpfile(void) {
  char *path = strdup("/tmp/oskar-XXXXXX");
  int fd = mkstemp(path);
  if (fd == -1)
    had_error("mkstemp failed: %s", strerror(errno));
  close(fd);

  strarray_push(&tmpfiles, path);
  return path;
}

static void run_subprocess(char **argv) {
  // If -### is given, dump the subprocess's command line.
  if (opts.hash_hash_hash) {
    fprintf(stderr, "%s", argv[0]);
    for (int i = 1; argv[i]; i++)
      fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
  }

  if (fork() == 0) {
    // Child process. Run a new command.
    execvp(argv[0], argv);
    fprintf(stderr, "exec failed: %s: %s\n", argv[0], strerror(errno));
    _exit(1);
  }

  // Wait for the child process to finish.
  int status;
  while (wait(&status) > 0);
  if (status != 0)
    exit(1);
}

static void run_cc1(int argc, char **argv, char *input, char *output) {
  char **args = calloc(argc + 10, sizeof(char *));
  memcpy(args, argv, argc * sizeof(char *));
  args[argc++] = "-cc1";

  if (input) {
    args[argc++] = "-cc1-input";
    args[argc++] = input;
  }

  if (output) {
    args[argc++] = "-cc1-output";
    args[argc++] = output;
  }

  run_subprocess(args);
}

// Print tokens to stdout. Used for -E.
static void print_tokens(Token *tok) {
  FILE *out = open_file(opts.o ? opts.o : "-");

  if (opts.verbose)
    fputs("(-E) with '-verbose', printed tokens have alternate format\n\n", stderr);

  int line = 1;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (opts.verbose) {
      const char* kind = token_kind_str[tok->kind];
      fprintf(out, "(%s) %.*s\n", kind, tok->len, tok->loc);
    } else {
      if (line > 1 && tok->at_bol)
        fprintf(out, "\n");
      if (tok->has_space && !tok->at_bol)
        fprintf(out, " ");
      fprintf(out, "%.*s", tok->len, tok->loc);
    }
    line++;
  }
  fprintf(out, "\n");
}

static bool in_std_include_path(char *path) {
  for (int i = 0; i < std_include_paths.len; i++) {
    char *dir = std_include_paths.data[i];
    int len = strlen(dir);
    if (strncmp(dir, path, len) == 0 && path[len] == '/')
      return true;
  }
  return false;
}

// If -M options is given, the compiler write a list of input files to
// stdout in a format that "make" command can read. This feature is
// used to automate file dependency management.
static void print_dependencies(void) {
  char *path;
  if (opts.MF)
    path = opts.MF;
  else if (opts.MD)
    path = replace_extn(opts.o ? opts.o : base_file, ".d");
  else if (opts.o)
    path = opts.o;
  else
    path = "-";

  FILE *out = open_file(path);
  if (opts.MT)
    fprintf(out, "%s:", opts.MT);
  else
    fprintf(out, "%s:", quote_makefile(replace_extn(base_file, ".o")));

  File **files = get_input_files();

  for (int i = 0; files[i]; i++) {
    if (opts.MMD && in_std_include_path(files[i]->name))
      continue;
    fprintf(out, " \\\n  %s", files[i]->name);
  }

  fprintf(out, "\n\n");

  if (opts.MP) {
    for (int i = 1; files[i]; i++) {
      if (opts.MMD && in_std_include_path(files[i]->name))
        continue;
      fprintf(out, "%s:\n\n", quote_makefile(files[i]->name));
    }
  }
}

static Token *must_tokenize_file(char *path) {
  Token *tok = tokenize_file(path);
  if (!tok)
    had_error("%s: %s", path, strerror(errno));
  return tok;
}

static Token *append_tokens(Token *tok1, Token *tok2) {
  if (!tok1 || tok1->kind == TK_EOF)
    return tok2;

  Token *t = tok1;
  while (t->next->kind != TK_EOF)
    t = t->next;
  t->next = tok2;
  return tok1;
}

static void cc1(void) {
  Token *tok = NULL;

  // Process -include option
  for (int i = 0; i < opts.include.len; i++) {
    char *incl = opts.include.data[i];

    char *path;
    if (file_exists(incl)) {
      path = incl;
    } else {
      path = search_include_paths(incl);
      if (!path)
        had_error("-include: %s: %s", incl, strerror(errno));
    }

    Token *tok2 = must_tokenize_file(path);
    tok = append_tokens(tok, tok2);
  }

  // Tokenize and parse.
  Token *tok2 = must_tokenize_file(base_file);
  tok = append_tokens(tok, tok2);
  tok = preprocess(tok);

  // If -M or -MD are given, print file dependencies.
  if (opts.M || opts.MD) {
    print_dependencies();
    if (opts.M)
      return;
  }

  // If -E is given, print out preprocessed C code as a result.
  if (opts.E) {
    print_tokens(tok);
    return;
  }

  Obj *prog = parse(tok);

  // Open a temporary output buffer.
  char *buf;
  size_t buflen;
  FILE *output_buf = open_memstream(&buf, &buflen);

  // Traverse the AST to emit assembly.
  codegen(prog, output_buf);
  fclose(output_buf);

  // Write the asembly text to a file.
  FILE *out = open_file(output_file);
  fwrite(buf, buflen, 1, out);
  fclose(out);
}

static void assemble(char *input, char *output) {
  char *cmd[] = {"as", "-c", input, "-o", output, NULL};
  run_subprocess(cmd);
}

static char *find_file(char *pattern) {
  char *path = NULL;
  glob_t buf = {};
  glob(pattern, 0, NULL, &buf);
  if (buf.gl_pathc > 0)
    path = strdup(buf.gl_pathv[buf.gl_pathc - 1]);
  globfree(&buf);
  return path;
}

// Returns true if a given file exists.
bool file_exists(char *path) {
  struct stat st;
  return !stat(path, &st);
}

char* tryLibPathEnv(char* envvar, char* file) {
  int raw_len = strlen(envvar);
  envvar[raw_len] = ':';
  int file_len = strlen(file);
  char* buf = malloc(raw_len+file_len+1); //includes len of "/" + strlen(file)
  buf[raw_len+file_len] = 0;
  int start = 0;
  for (int i = 0; i <= raw_len; i++) {
    if (envvar[i] == ':') goto try_sect;
    continue;
    try_sect: {
      memcpy(buf, envvar+start, i-start);
      memcpy(buf+i-start+1, file, file_len);
      buf[i-start] = '/';
      buf[i-start+file_len+1] = 0;
      if (find_file(buf)) {
        char *res = malloc(i-start+1);
        memcpy(res, buf, i-start);
        free(buf);
        envvar[raw_len] = 0;
        res[i-start] = 0;
        return res;
      }
      start = i+1;
      if (i == raw_len-1) break;
    }
  }
  free(buf);
  envvar[raw_len] = 0;
  return NULL;
}

//ugh... hard coded paths? (rookie)
static char *find_libpath(void) {
  char* libpath = getenv("LIBRARY_PATH");
  if (!libpath) {
    if (opts.verbose)
      fputs("LIBRARY_PATH not set (trying common paths)\n", stderr);
    goto hardcoded;
  }

  char* match = tryLibPathEnv(libpath, "crti.o");
  if (!match) {
    if (opts.verbose)
      fputs("could not find crti.o in LIBRARY PATH (trying common paths)\n", stderr);
    goto hardcoded;
  }
  if (opts.verbose) puts("found crti.o");
  return match;

  hardcoded: {
    fputs("LIBRARY_PATH not set\n", stderr);
    if (file_exists("/usr/lib/x86_64-linux-gnu/crti.o"))
      return "/usr/lib/x86_64-linux-gnu";
    if (file_exists("/usr/lib64/crti.o"))
      return "/usr/lib64";
    had_error("library path is not found");
  }
}

static char *find_gcc_libpath(void) {
  char* ld_path = getenv("LD_LIBRARY_PATH");
  if (ld_path != NULL) {
    char* match = tryLibPathEnv(ld_path, "crtbegin.o");
    if (match) return match;
    fputs("crtbegin.o not found in LD_LIBRARY_PATH\n", stderr);
  } else if (opts.verbose)
    fputs("LD_LIBRARY_PATH not set\n", stderr);

  if (opts.verbose)
    puts("trying common paths (for crtbegin.o)");
  char* hardcoded = "/usr/lib/gcc/*/crtbegin.o";
  char* path = find_file(hardcoded);
  if (path) {
    char* duped = strdup(dirname(path));
    free(path);
    return duped;
  }
  had_error("gcc library path (where 'crtbegin.o' is) is not found");
}

static void run_linker(StringArray *inputs, char *output) {
  StringArray arr = {};

  strarray_push(&arr, "ld");
  strarray_push(&arr, "-o");
  strarray_push(&arr, output);
  strarray_push(&arr, "-m");
  strarray_push(&arr, "elf_x86_64");

  char *libpath = find_libpath();
  char *gcc_libpath = find_gcc_libpath();

  if (opts.linkage == LINK_SHARED) {
    strarray_push(&arr, format("%s/crti.o", libpath));
    strarray_push(&arr, format("%s/crtbeginS.o", gcc_libpath));
  } else {
    strarray_push(&arr, format("%s/crt1.o", libpath));
    strarray_push(&arr, format("%s/crti.o", libpath));
    strarray_push(&arr, format("%s/crtbegin.o", gcc_libpath));
  }

  strarray_push(&arr, format("-L%s", gcc_libpath));
  strarray_push(&arr, "-L/usr/lib/x86_64-linux-gnu");
  strarray_push(&arr, "-L/usr/lib64");
  strarray_push(&arr, "-L/lib64");
  strarray_push(&arr, "-L/usr/lib/x86_64-linux-gnu");
  strarray_push(&arr, "-L/usr/lib/x86_64-pc-linux-gnu");
  strarray_push(&arr, "-L/usr/lib/x86_64-redhat-linux");
  strarray_push(&arr, "-L/usr/lib");
  strarray_push(&arr, "-L/lib");

  char *ldflags = getenv("LDFLAGS");
  if (ldflags) {
    int start = 0;
    int f_len = strlen(ldflags);
    for (int i = 0; i < f_len; i++) {
      if (ldflags[i] == ' ' && start < i) {
        ldflags[i] = 0;
        strarray_push(&arr, format("-L%s", ldflags+start));
        start = i+1;
      }
    }
    if (start < f_len)
      strarray_push(&arr, format("-L%s", ldflags+start));
  } else if (opts.verbose)
    fputs("LDFLAGS is not set\n", stderr);

  char* library_path = getenv("LIBRARY_PATH");
  if (library_path) {
    size_t start = 0;
    for (size_t i = 0; i < strlen(library_path); i++) {
      if (library_path[i] == ':' && start < i) {
        library_path[i] = 0;
        strarray_push(&arr, format("-L%s", library_path+start));
        library_path[i] = ':';
        start = i+1;
      }
    }
    if (start < strlen(library_path))
      strarray_push(&arr, format("-L%s", library_path+start));
  } else if (opts.verbose)
    fputs("LIBRARY_PATH is not set\n", stderr);

  if (opts.linkage != LINK_STATIC) {
    strarray_push(&arr, "-dynamic-linker");
    strarray_push(&arr, "/lib64/ld-linux-x86-64.so.2");
  }

  for (int i = 0; i < ld_extra_args.len; i++)
    strarray_push(&arr, ld_extra_args.data[i]);

  for (int i = 0; i < inputs->len; i++)
    strarray_push(&arr, inputs->data[i]);

  if (opts.linkage == LINK_STATIC) {
    strarray_push(&arr, "--start-group");
    strarray_push(&arr, "-lgcc");
    strarray_push(&arr, "-lgcc_eh");
    strarray_push(&arr, "-lc");
    strarray_push(&arr, "--end-group");
  } else {
    strarray_push(&arr, "-lc");
    strarray_push(&arr, "-lgcc");
    strarray_push(&arr, "--as-needed");
    strarray_push(&arr, "-lgcc_s");
    strarray_push(&arr, "--no-as-needed");
  }

  if (opts.linkage == LINK_SHARED)
    strarray_push(&arr, format("%s/crtendS.o", gcc_libpath));
  else
    strarray_push(&arr, format("%s/crtend.o", gcc_libpath));

  strarray_push(&arr, format("%s/crtn.o", libpath));
  strarray_push(&arr, NULL);

  if (opts.verbose) {
    puts("about to run:");
    for (int i = 0; i < arr.len; i++) if (arr.data[i])
      printf("\t%s %c\n", arr.data[i], (arr.data[i+1]) ? '\\' : ' ');
    puts("\n");
  }

  free(libpath);
  free(gcc_libpath);

  run_subprocess(arr.data);
}

static FileType get_file_type(char *filename) {
  if (opts.x != FILE_NONE)
    return opts.x;

  if (endswith(filename, ".a"))
    return FILE_AR;
  if (endswith(filename, ".so"))
    return FILE_DSO;
  if (endswith(filename, ".o"))
    return FILE_OBJ;
  if (endswith(filename, ".c"))
    return FILE_C;
  if (endswith(filename, ".s"))
    return FILE_ASM;

  had_error("<command line>: unknown file extension: %s", filename);
}

int main(int argc, char **argv) {
  atexit(cleanup);
  init_macros();
  parse_args(argc, argv);

  if (opts.cc1) {
    add_default_include_paths(argv[0]);
    cc1();
    return 0;
  }

  if (input_paths.len > 1 && opts.o && (opts.c || opts.S | opts.E))
    had_error("cannot specify '-o' with '-c,' '-S' or '-E' with multiple files");

  StringArray ld_args = {};

  for (int i = 0; i < input_paths.len; i++) {
    char *input = input_paths.data[i];

    if (!strncmp(input, "-l", 2)) {
      strarray_push(&ld_args, input);
      continue;
    }

    if (!strncmp(input, "-Wl,", 4)) {
      char *s = strdup(input + 4);
      char *arg = strtok(s, ",");
      while (arg) {
        strarray_push(&ld_args, arg);
        arg = strtok(NULL, ",");
      }
      continue;
    }

    char *output;
    if (opts.o)
      output = opts.o;
    else if (opts.S)
      output = replace_extn(input, ".s");
    else
      output = replace_extn(input, ".o");

    FileType type = get_file_type(input);

    // Handle .o or .a
    if (type == FILE_OBJ || type == FILE_AR || type == FILE_DSO) {
      strarray_push(&ld_args, input);
      continue;
    }

    // Handle .s
    if (type == FILE_ASM) {
      if (!opts.S)
        assemble(input, output);
      continue;
    }

    assert(type == FILE_C);

    // Just preprocess
    if (opts.E || opts.M) {
      run_cc1(argc, argv, input, NULL);
      continue;
    }

    // Compile
    if (opts.S) {
      run_cc1(argc, argv, input, output);
      continue;
    }

    // Compile and assemble
    if (opts.c) {
      char *tmp = create_tmpfile();
      run_cc1(argc, argv, input, tmp);
      assemble(tmp, output);
      continue;
    }

    // Compile, assemble and link
    char *tmp1 = create_tmpfile();
    char *tmp2 = create_tmpfile();
    run_cc1(argc, argv, input, tmp1);
    assemble(tmp1, tmp2);
    strarray_push(&ld_args, tmp2);
    continue;
  }

  if (ld_args.len > 0)
    run_linker(&ld_args, opts.o ? opts.o : "a.out");

  if (opts.run) {
    if (opts.consume_args) {
      puts("consume args");
    }

    strarray_push(&passed_args, NULL);
    char** args = malloc(sizeof(char *) * passed_args.len+1);
    args[0] = opts.o ? opts.o : "a.out";
    for (int i = 0; i < passed_args.len; i++)
      args[i+1] = passed_args.data[i];

    char cwd[PATH_MAX];
    assert(getcwd(cwd, sizeof(cwd)));
    size_t dir_len = strlen(cwd);
    cwd[dir_len] = '/';
    cwd[dir_len+strlen(args[0])+1] = 0;
    for (size_t i = 1; i < strlen(args[0])+1; i++)
      cwd[dir_len+i] = args[0][i-1];
    args[0] = cwd;
    assert(file_exists(args[0]));

    int rc = fork();
    if (rc == -1) {
      fprintf(stderr, "fork failed: %s\n", strerror(errno));
      _exit(1);
    }

    if (rc == 0) {
      execvp(args[0], args);
      fprintf(stderr, "exec failed: %s: %s\n", args[0], strerror(errno));
      free(args);
      remove(opts.o ? opts.o : "a.out");
      _exit(1);
    }

    // Wait for the child process to finish.
    int status;
    while (wait(&status) > 0);
    if (status != 0) return 1;
    remove(args[0]);
    free(args);
  }

  return 0;
}
