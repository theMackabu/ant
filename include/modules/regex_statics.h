#ifdef REGEXP_STATIC
REGEXP_STATIC(INPUT,         "input",        "$_")
REGEXP_STATIC(LAST_MATCH,    "lastMatch",    "$&")
REGEXP_STATIC(LAST_PAREN,    "lastParen",    "$+")
REGEXP_STATIC(LEFT_CONTEXT,  "leftContext",  "$`")
REGEXP_STATIC(RIGHT_CONTEXT, "rightContext", "$'")
REGEXP_STATIC(D1,            "$1",           "")
REGEXP_STATIC(D2,            "$2",           "")
REGEXP_STATIC(D3,            "$3",           "")
REGEXP_STATIC(D4,            "$4",           "")
REGEXP_STATIC(D5,            "$5",           "")
REGEXP_STATIC(D6,            "$6",           "")
REGEXP_STATIC(D7,            "$7",           "")
REGEXP_STATIC(D8,            "$8",           "")
REGEXP_STATIC(D9,            "$9",           "")
#undef REGEXP_STATIC
#endif
