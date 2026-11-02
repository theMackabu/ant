export const base64Seeds = [
  {
    "name": "bad_bits",
    "url": false,
    "lastChunkHandling": "strict",
    "step": 1,
    "bytes": [
      81,
      82,
      61,
      61
    ],
    "text": "QR=="
  },
  {
    "name": "binary",
    "url": false,
    "lastChunkHandling": "stop-before-partial",
    "step": 4,
    "bytes": [
      0,
      255,
      254,
      128,
      127,
      1
    ],
    "text": "\u0000ÿþ\u0001"
  },
  {
    "name": "padded",
    "url": false,
    "lastChunkHandling": "strict",
    "step": 1,
    "bytes": [
      81,
      81,
      61,
      61
    ],
    "text": "QQ=="
  },
  {
    "name": "padded_space",
    "url": false,
    "lastChunkHandling": "stop-before-partial",
    "step": 2,
    "bytes": [
      32,
      81,
      81,
      32,
      61,
      32,
      61,
      32
    ],
    "text": " QQ = = "
  },
  {
    "name": "stop_partial",
    "url": false,
    "lastChunkHandling": "stop-before-partial",
    "step": 1,
    "bytes": [
      81,
      85,
      74,
      68,
      82,
      65
    ],
    "text": "QUJDRA"
  },
  {
    "name": "text",
    "url": false,
    "lastChunkHandling": "loose",
    "step": 1,
    "bytes": [
      104,
      101,
      108,
      108,
      111,
      32,
      119,
      111,
      114,
      108,
      100
    ],
    "text": "hello world"
  },
  {
    "name": "url",
    "url": true,
    "lastChunkHandling": "loose",
    "step": 1,
    "bytes": [
      45,
      95,
      45,
      95,
      65,
      65
    ],
    "text": "-_-_AA"
  }
];

export const http1Seeds = [
  {
    "name": "absolute_http10",
    "seed": 31,
    "request": "GET http://example.com/x HTTP/1.0\r\nUser-Agent: t\r\n\r\n"
  },
  {
    "name": "bad_target",
    "seed": 9,
    "request": "GET / HTTP/1.1\r\n\r\n"
  },
  {
    "name": "duplicate_content_length",
    "seed": 2,
    "request": "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab"
  },
  {
    "name": "get_root",
    "seed": 0,
    "request": "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"
  },
  {
    "name": "options_star",
    "seed": 4,
    "request": "OPTIONS * HTTP/1.1\r\nHost: x\r\nX-Empty:\r\nX-Spaces:   v   \r\n\r\n"
  },
  {
    "name": "pipelined",
    "seed": 1,
    "request": "GET /a HTTP/1.1\r\nHost: a\r\n\r\nGET /b HTTP/1.1\r\nHost: b\r\nConnection: close\r\n\r\n"
  },
  {
    "name": "pipelined_body",
    "seed": 5,
    "request": "PUT /k HTTP/1.1\r\nHost: 127.0.0.1:443\r\nConnection: keep-alive\r\nContent-Length: 3\r\n\r\nabcGET / HTTP/1.1\r\n\r\n"
  },
  {
    "name": "post_chunked",
    "seed": 3,
    "request": "POST /up HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n"
  },
  {
    "name": "post_content_length",
    "seed": 7,
    "request": "POST /submit?x=1 HTTP/1.1\r\nHost: localhost:8080\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world"
  }
];

export const jsSeeds = [
  {
    "name": "arrows_closures",
    "mode": "script",
    "strict": true,
    "source": "const f = (a, { b } = {}, [c] = []) => async () => (a + b) * c; new.target; (() => this)();"
  },
  {
    "name": "async_gen",
    "mode": "script",
    "strict": false,
    "source": "async function* g(a = 1, ...r) { for await (const x of r) yield* [x]; try { await a; } catch { } finally { return; } }"
  },
  {
    "name": "class_fields",
    "mode": "script",
    "strict": false,
    "source": "class A extends B { #x = 1; static #y; get z() { return this.#x ?? super.z; } static { this.k = 1; } }"
  },
  {
    "name": "deep_nesting",
    "mode": "script",
    "strict": false,
    "source": "(((((((((((((((((((((((((((((((((((1)))))))))))))))))))))))))))))))))));[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]];"
  },
  {
    "name": "destructure",
    "mode": "script",
    "strict": true,
    "source": "const { a, b: [c = 1, ...d], ...e } = o; let [x, , y] = arr; ({ a } = { a: 1 });"
  },
  {
    "name": "eval_scope",
    "mode": "eval",
    "strict": false,
    "source": "var v = 1; function f() { return typeof arguments; } with (o) { v }"
  },
  {
    "name": "generators_spread",
    "mode": "script",
    "strict": false,
    "source": "function* f() { const x = yield; yield* f(); } [...f(), ...[1, 2]]; f(...args, ...more);"
  },
  {
    "name": "getters_computed",
    "mode": "script",
    "strict": false,
    "source": "const o = { get [k]() { return 1 }, set v(x) {}, async *m() {}, [Symbol.iterator]: null, __proto__: p };"
  },
  {
    "name": "labels_loops",
    "mode": "script",
    "strict": false,
    "source": "outer: for (let i = 0; i < 3; i++) { inner: while (true) { if (i) continue outer; break inner; } } do x--; while (x > 0)"
  },
  {
    "name": "module",
    "mode": "module",
    "strict": false,
    "source": "import d, { a as b } from \"m\"; export const x = await import(\"y\"); export default class {} export * as ns from \"z\";"
  },
  {
    "name": "octal_escapes",
    "mode": "script",
    "strict": false,
    "source": "\"\\0\\1\\12\\377\\8\"; \"\\u{10FFFF}\\uD83D\\uDE00\\x7f\";"
  },
  {
    "name": "optional_chain",
    "mode": "script",
    "strict": false,
    "source": "a?.b?.[c]?.(d) ?? e; a ||= b; a &&= c; a ??= d; x **= 2;"
  },
  {
    "name": "regex_literals",
    "mode": "script",
    "strict": false,
    "source": "const r = /(?<y>\\d{4})-(?:a|b)+[^\\]/]/giu; s.replace(/x/g, \"$1\");"
  },
  {
    "name": "repl",
    "mode": "repl",
    "strict": false,
    "source": "let a = 1; await Promise.resolve(a); class C {}"
  },
  {
    "name": "switch_try",
    "mode": "script",
    "strict": false,
    "source": "switch (x) { case 1: try { throw new Error } catch (e) { y = e } finally { z() } default: ; }"
  },
  {
    "name": "template",
    "mode": "script",
    "strict": false,
    "source": "const t = tag`a${1}b${`nested ${x}`}\\u{41}\\x41\\101`;"
  },
  {
    "name": "using",
    "mode": "script",
    "strict": false,
    "source": "{ using r = res(); await using q = res2(); }"
  }
];

export const numbersSeeds = [
  {
    "name": "binary_octal",
    "bits": "0x4330000000000000",
    "digits": 21,
    "text": "Â 0b1010ã"
  },
  {
    "name": "denormal",
    "bits": "0x0000000000000001",
    "digits": 100,
    "text": " .5e-323"
  },
  {
    "name": "fixed_big",
    "bits": "0x444b1ae4d6e2ef50",
    "digits": 100,
    "text": " 1e21"
  },
  {
    "name": "hex",
    "bits": "0x41e0000000000000",
    "digits": 10,
    "text": "  0x1F\t"
  },
  {
    "name": "inf",
    "bits": "0x7ff0000000000000",
    "digits": 2,
    "text": "-Infinity"
  },
  {
    "name": "int_boundary",
    "bits": "0x41dfffffffc00000",
    "digits": 0,
    "text": " 2147483647 "
  },
  {
    "name": "junk",
    "bits": "0x3fb999999999999a",
    "digits": 3,
    "text": "  -.1e-x"
  },
  {
    "name": "max",
    "bits": "0x7fefffffffffffff",
    "digits": 20,
    "text": " 1.7976931348623157e308"
  },
  {
    "name": "nan",
    "bits": "0x7ff8000000000001",
    "digits": 0,
    "text": "NaN"
  },
  {
    "name": "neg_zero",
    "bits": "0x8000000000000000",
    "digits": 0,
    "text": "-0"
  },
  {
    "name": "pi",
    "bits": "0x400921fb54442d18",
    "digits": 5,
    "text": " 3.14159e+0 "
  }
];

export const regexpSeeds = [
  {
    "name": "anchors",
    "flags": "m",
    "source": "^a$|\\bword\\B"
  },
  {
    "name": "backrefs",
    "flags": "i",
    "source": "(a)|\\1b\\2"
  },
  {
    "name": "brace_not_quantifier",
    "flags": "",
    "source": "a{,5}*x{a}{"
  },
  {
    "name": "case_insensitive",
    "flags": "iu",
    "source": "[a-z\\u017F\\u212A]"
  },
  {
    "name": "class_identity_B",
    "flags": "",
    "source": "[\\B[[:alpha:]]"
  },
  {
    "name": "classes",
    "flags": "u",
    "source": "[\\p{L}\\d_-]+\\P{Script=Greek}"
  },
  {
    "name": "dotall",
    "flags": "s",
    "source": "a.b[^]"
  },
  {
    "name": "groups",
    "flags": "",
    "source": "(?<year>\\d{4})-(?<m>\\d\\d)\\k<year>"
  },
  {
    "name": "identity_p_escape",
    "flags": "",
    "source": "\\P{Scri}[\\p{L}]"
  },
  {
    "name": "identity_u_brace",
    "flags": "",
    "source": "[a-\\u{41}]\\u{2}"
  },
  {
    "name": "legacy_escapes",
    "flags": "",
    "source": "\\c\\x\\u{41}\\8]{"
  },
  {
    "name": "legacy_octal_backref",
    "flags": "",
    "source": "(a)|\\1b\\2\\8"
  },
  {
    "name": "lookaround",
    "flags": "",
    "source": "(?<=a)b(?=c)(?!d)(?<!e)"
  },
  {
    "name": "modifiers",
    "flags": "",
    "source": "(?i:a)(?-i:b)"
  },
  {
    "name": "quantifiers",
    "flags": "",
    "source": "a{2,5}?b*+c{3}d{0,}"
  },
  {
    "name": "sets",
    "flags": "v",
    "source": "[\\p{L}--[a-z]]&&[\\q{abc|d}]"
  },
  {
    "name": "surrogate_pair_unicode",
    "flags": "v",
    "source": "[\\uD83D\\uDE00]\\uD83D\\uDE00"
  },
  {
    "name": "unicode_escapes",
    "flags": "u",
    "source": "\\u{1F600}\\uD83D\\uDE00."
  }
];

export const textSeeds = [
  {
    "name": "bom",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      239,
      187,
      191,
      66,
      79,
      77
    ]
  },
  {
    "name": "control_chars",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      1,
      31,
      34,
      92,
      127,
      226,
      128,
      168
    ]
  },
  {
    "name": "escape_tail",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      97,
      98,
      99,
      92,
      48
    ]
  },
  {
    "name": "escapes",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      92,
      110,
      92,
      116,
      92,
      120,
      52,
      49,
      92,
      117,
      48,
      48,
      52,
      49,
      92,
      117,
      123,
      49,
      70,
      54,
      48,
      48,
      125,
      92,
      117,
      68,
      56,
      51,
      68,
      92,
      117,
      68,
      69,
      48,
      48,
      92,
      49,
      48,
      49,
      92,
      48,
      92,
      56,
      92,
      10
    ]
  },
  {
    "name": "form",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      97,
      32,
      98,
      38,
      99,
      61,
      100,
      37,
      50,
      48,
      101,
      43,
      102,
      47,
      0,
      127
    ]
  },
  {
    "name": "invalid_utf8",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      97,
      255,
      192,
      175,
      237,
      160,
      128,
      98,
      226,
      130
    ]
  },
  {
    "name": "lone_surrogate",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      120,
      237,
      160,
      128,
      121,
      237,
      191,
      191
    ]
  },
  {
    "name": "octal_tail",
    "quote": "'",
    "ignoreBOM": false,
    "bytes": [
      120,
      92,
      49,
      50
    ]
  },
  {
    "name": "percent",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      37,
      52,
      49,
      37,
      122,
      122,
      37,
      52,
      37,
      101,
      50,
      37,
      56,
      50,
      37,
      97,
      99,
      43,
      37
    ]
  },
  {
    "name": "utf8_mix",
    "quote": "\"",
    "ignoreBOM": false,
    "bytes": [
      104,
      195,
      169,
      108,
      108,
      111,
      32,
      226,
      130,
      172,
      32,
      240,
      159,
      152,
      128
    ]
  }
];

export const websocketSeeds = [
  {
    "name": "binary_len126_header",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "~\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000"
  },
  {
    "name": "close_1000",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "\u0005\u0003èbye"
  },
  {
    "name": "fragmented",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "\u0001\u0003abc\u0003def"
  },
  {
    "name": "len127_too_big",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": 64,
    "frames": "\u0000\u0000\u0000\u0000\u0000\u0001\u0000\u0000"
  },
  {
    "name": "masked_text_hello",
    "requireMask": true,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "7ú!=MQX"
  },
  {
    "name": "ping",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "\u0004ping"
  },
  {
    "name": "ping_without_fin",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "\t\u0000"
  },
  {
    "name": "reserved_opcode",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "\u0002hi"
  },
  {
    "name": "rsv1_text",
    "requireMask": false,
    "allowRsv1": true,
    "maxPayload": null,
    "frames": "Á\u0003abc"
  },
  {
    "name": "rsv2_set",
    "requireMask": false,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "±\u0001x"
  },
  {
    "name": "unmasked_when_required",
    "requireMask": true,
    "allowRsv1": false,
    "maxPayload": null,
    "frames": "\u0000"
  }
];
