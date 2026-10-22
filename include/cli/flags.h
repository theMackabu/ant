#ifdef CLI_FLAG
CLI_FLAG(EVAL,              STR,  "e",  "eval",              "<script>",   "evaluate script")
CLI_FLAG(REPL,              STR,  NULL, "repl",              "<script>",   "start REPL after evaluating script")
CLI_FLAG(INPUT_TYPE,        STR,  NULL, "input-type",        "<type>",     "set string input type: commonjs or module")
CLI_FLAG(PRINT,             LIT,  "p",  "print",             NULL,         "evaluate script and print result")
CLI_FLAG(WATCH,             LIT,  "w",  "watch",             NULL,         "restart process when entry file changes")
CLI_FLAG(WEB_COMPAT,        LIT,  NULL, "web-compat",        NULL,         "browser-compatible mode: location, relative URLs, browser package resolution")
CLI_FLAG(LOCATION,          STR,  NULL, "location",          "<href>",     "set globalThis.location and the base URL for relative fetches")
CLI_FLAG(NO_CLEAR_SCREEN,   LIT,  NULL, "no-clear-screen",   NULL,         "keep output when restarting in watch mode")
CLI_FLAG(JITLESS,           LIT,  NULL, "jitless",           NULL,         "run everything in the interpreter, without compiling to native code")
CLI_FLAG(LOCALSTORAGE_FILE, FILE, NULL, "localstorage-file", "<path>",     "file path for localStorage persistence")
CLI_FLAG(CRON_TITLE,        STR,  NULL, "cron-title",        "<title>",    NULL)
CLI_FLAG(CRON_PERIOD,       STR,  NULL, "cron-period",       "<schedule>", NULL)
CLI_FLAG(VERSION,           LIT,  "v",  "version",           NULL,         "display version information and exit")
CLI_FLAG(VERSION_RAW,       LIT,  NULL, "version-raw",       NULL,         "raw version number for scripts")
CLI_FLAG(VERSION_CHANNEL,   LIT,  NULL, "version-channel",   NULL,         "release channel (stable or canary)")
CLI_FLAG(HELP,              LIT,  "h",  "help",              NULL,         "display this help and exit")
#undef CLI_FLAG
#endif
