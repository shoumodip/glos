:i count 61
:b testcase 20
integers/basics.glos
:i returncode 0
:b stdout 70
69
420
69
420
69
1
0
0
1
1
0
1
0
0
1
1
0
1
0
1
0
69
420
69
420
69
420

:b stderr 0

:b testcase 26
integers/signed-types.glos
:i returncode 0
:b stdout 26
69
1
420
2
1337
4
80085
8

:b stderr 0

:b testcase 28
integers/unsigned-types.glos
:i returncode 0
:b stdout 26
69
1
420
2
1337
4
80085
8

:b stderr 0

:b testcase 34
integers/unsigned-comparisons.glos
:i returncode 0
:b stdout 29
1
0
18446744073709551615
1
0

:b stderr 0

:b testcase 39
integers/untyped-literal-auto-cast.glos
:i returncode 0
:b stdout 36
69
420
1337
80085
69
420
1337
80085

:b stderr 0

:b testcase 55
integers/error-untyped-literal-auto-cast-too-large.glos
:i returncode 1
:b stdout 0

:b stderr 118
integers/error-untyped-literal-auto-cast-too-large.glos:1:12: ERROR: Integer literal '420' is too large for type 'i8'

:b testcase 20
booleans/basics.glos
:i returncode 0
:b stdout 64
1
0
0
1
69
420
1
69
420
0
69
0
69
0
69
1
69
1
69
420
1
69
420
0

:b stderr 0

:b testcase 22
conditions/basics.glos
:i returncode 0
:b stdout 18
69
420
1337
80085

:b stderr 0

:b testcase 42
conditions/error-expected-block-or-if.glos
:i returncode 1
:b stdout 0

:b stderr 90
conditions/error-expected-block-or-if.glos:2:21: ERROR: Expected '{' or 'if', got integer

:b testcase 50
conditions/error-expected-condition-type-bool.glos
:i returncode 1
:b stdout 0

:b stderr 95
conditions/error-expected-condition-type-bool.glos:2:8: ERROR: Expected type 'bool', got 'i64'

:b testcase 28
global-variables/basics.glos
:i returncode 0
:b stdout 12
69
420
1337

:b stderr 0

:b testcase 37
global-variables/error-undefined.glos
:i returncode 1
:b stdout 0

:b stderr 78
global-variables/error-undefined.glos:2:11: ERROR: Undefined identifier 'foo'

:b testcase 40
global-variables/error-redefinition.glos
:i returncode 1
:b stdout 0

:b stderr 149
global-variables/error-redefinition.glos:2:5: ERROR: Redefinition of identifier 'x'
global-variables/error-redefinition.glos:1:5: NOTE: Defined here

:b testcase 63
global-variables/error-assignment-definition-type-mismatch.glos
:i returncode 1
:b stdout 0

:b stderr 109
global-variables/error-assignment-definition-type-mismatch.glos:1:13: ERROR: Expected type 'i64', got 'bool'

:b testcase 56
global-variables/error-cannot-define-with-type-unit.glos
:i returncode 1
:b stdout 0

:b stderr 107
global-variables/error-cannot-define-with-type-unit.glos:3:5: ERROR: Cannot define variable with type '()'

:b testcase 22
assignment/basics.glos
:i returncode 0
:b stdout 7
69
420

:b stderr 0

:b testcase 35
assignment/error-type-mismatch.glos
:i returncode 1
:b stdout 0

:b stderr 80
assignment/error-type-mismatch.glos:4:9: ERROR: Expected type 'i64', got 'bool'

:b testcase 43
assignment/error-cannot-take-reference.glos
:i returncode 1
:b stdout 0

:b stderr 101
assignment/error-cannot-take-reference.glos:2:5: ERROR: Cannot take reference to value not in memory

:b testcase 17
loops/basics.glos
:i returncode 0
:b stdout 20
0
1
2
3
4
5
6
7
8
9

:b stderr 0

:b testcase 26
loops/init-expression.glos
:i returncode 0
:b stdout 20
0
1
2
3
4
5
6
7
8
9

:b stderr 0

:b testcase 35
loops/init-variable-definition.glos
:i returncode 0
:b stdout 20
0
1
2
3
4
5
6
7
8
9

:b stderr 0

:b testcase 29
loops/init-but-no-update.glos
:i returncode 0
:b stdout 20
0
1
2
3
4
5
6
7
8
9

:b stderr 0

:b testcase 19
loops/infinite.glos
:i returncode 0
:b stdout 20
0
1
2
3
4
5
6
7
8
9

:b stderr 0

:b testcase 45
loops/error-expected-condition-type-bool.glos
:i returncode 1
:b stdout 0

:b stderr 90
loops/error-expected-condition-type-bool.glos:2:9: ERROR: Expected type 'bool', got 'i64'

:b testcase 68
loops/error-expected-condition-after-assignment-expression-init.glos
:i returncode 1
:b stdout 0

:b stderr 104
loops/error-expected-condition-after-assignment-expression-init.glos:3:15: ERROR: Expected ';', got '{'

:b testcase 66
loops/error-expected-condition-after-variable-definition-init.glos
:i returncode 1
:b stdout 0

:b stderr 102
loops/error-expected-condition-after-variable-definition-init.glos:2:19: ERROR: Expected ';', got '{'

:b testcase 27
local-variables/basics.glos
:i returncode 0
:b stdout 12
69
420
1337

:b stderr 0

:b testcase 30
local-variables/shadowing.glos
:i returncode 0
:b stdout 7
69
420

:b stderr 0

:b testcase 62
local-variables/error-assignment-definition-type-mismatch.glos
:i returncode 1
:b stdout 0

:b stderr 108
local-variables/error-assignment-definition-type-mismatch.glos:2:17: ERROR: Expected type 'i64', got 'bool'

:b testcase 50
local-variables/error-undefined-outside-scope.glos
:i returncode 1
:b stdout 0

:b stderr 89
local-variables/error-undefined-outside-scope.glos:7:11: ERROR: Undefined identifier 'x'

:b testcase 69
local-variables/error-undefined-outside-scope-despite-same-depth.glos
:i returncode 1
:b stdout 0

:b stderr 108
local-variables/error-undefined-outside-scope-despite-same-depth.glos:6:15: ERROR: Undefined identifier 'x'

:b testcase 55
local-variables/error-cannot-define-with-type-unit.glos
:i returncode 1
:b stdout 0

:b stderr 106
local-variables/error-cannot-define-with-type-unit.glos:4:9: ERROR: Cannot define variable with type '()'

:b testcase 44
functions/no-arguments-no-return-basics.glos
:i returncode 0
:b stdout 14
69
420
69
420

:b stderr 0

:b testcase 49
functions/no-arguments-no-return-first-class.glos
:i returncode 0
:b stdout 14
69
420
69
420

:b stderr 0

:b testcase 45
functions/yes-arguments-no-return-basics.glos
:i returncode 0
:b stdout 7
69
420

:b stderr 0

:b testcase 50
functions/yes-arguments-no-return-first-class.glos
:i returncode 0
:b stdout 6
69
69

:b stderr 0

:b testcase 46
functions/yes-arguments-yes-return-basics.glos
:i returncode 0
:b stdout 3
69

:b stderr 0

:b testcase 51
functions/yes-arguments-yes-return-first-class.glos
:i returncode 0
:b stdout 4
420

:b stderr 0

:b testcase 40
functions/arguments-as-local-memory.glos
:i returncode 0
:b stdout 14
69
420
69
420

:b stderr 0

:b testcase 31
functions/nested-functions.glos
:i returncode 0
:b stdout 7
69
420

:b stderr 0

:b testcase 26
functions/return-unit.glos
:i returncode 0
:b stdout 3
69

:b stderr 0

:b testcase 30
functions/return-not-unit.glos
:i returncode 0
:b stdout 3
69

:b stderr 0

:b testcase 24
functions/anonymous.glos
:i returncode 0
:b stdout 12
69
420
1337

:b stderr 0

:b testcase 44
functions/error-argument-count-mismatch.glos
:i returncode 1
:b stdout 0

:b stderr 85
functions/error-argument-count-mismatch.glos:4:8: ERROR: Expected 2 arguments, got 0

:b testcase 43
functions/error-argument-type-mismatch.glos
:i returncode 1
:b stdout 0

:b stderr 88
functions/error-argument-type-mismatch.glos:4:9: ERROR: Expected type 'i64', got 'bool'

:b testcase 51
functions/error-function-literal-is-not-memory.glos
:i returncode 1
:b stdout 0

:b stderr 109
functions/error-function-literal-is-not-memory.glos:4:6: ERROR: Cannot take reference to value not in memory

:b testcase 74
functions/error-cannot-call-pointer-to-function-without-dereferencing.glos
:i returncode 1
:b stdout 0

:b stderr 144
functions/error-cannot-call-pointer-to-function-without-dereferencing.glos:6:5: ERROR: Cannot call type '&fn ()' without dereferencing it first

:b testcase 68
functions/error-nested-functions-outside-identifier-used-inside.glos
:i returncode 1
:b stdout 0

:b stderr 107
functions/error-nested-functions-outside-identifier-used-inside.glos:5:15: ERROR: Undefined identifier 'x'

:b testcase 68
functions/error-nested-functions-inside-identifier-used-outside.glos
:i returncode 1
:b stdout 0

:b stderr 107
functions/error-nested-functions-inside-identifier-used-outside.glos:6:11: ERROR: Undefined identifier 'x'

:b testcase 20
pointers/basics.glos
:i returncode 0
:b stdout 38
69
420
69
420
420
420
420
69
420
1337

:b stderr 0

:b testcase 25
pointers/arithmetics.glos
:i returncode 0
:b stdout 2
1

:b stderr 0

:b testcase 39
pointers/multiple-level-type-parse.glos
:i returncode 0
:b stdout 7
69
420

:b stderr 0

:b testcase 41
pointers/error-cannot-take-reference.glos
:i returncode 1
:b stdout 0

:b stderr 99
pointers/error-cannot-take-reference.glos:2:6: ERROR: Cannot take reference to value not in memory

:b testcase 48
pointers/error-dereference-expected-pointer.glos
:i returncode 1
:b stdout 0

:b stderr 94
pointers/error-dereference-expected-pointer.glos:2:6: ERROR: Expected pointer type, got 'i64'

:b testcase 50
pointers/error-cannot-dereference-raw-pointer.glos
:i returncode 1
:b stdout 0

:b stderr 95
pointers/error-cannot-dereference-raw-pointer.glos:1:10: ERROR: Cannot dereference raw pointer

:b testcase 21
type-cast/basics.glos
:i returncode 0
:b stdout 10
1
0
1
0
1

:b stderr 0

:b testcase 85
type-cast/error-pointers-can-only-be-casted-from-other-pointers-and-u64-integers.glos
:i returncode 1
:b stdout 0

:b stderr 137
type-cast/error-pointers-can-only-be-casted-from-other-pointers-and-u64-integers.glos:3:9: ERROR: Cannot cast type 'bool' to type '&i64'

:b testcase 83
type-cast/error-pointers-can-only-be-casted-to-other-pointers-and-u64-integers.glos
:i returncode 1
:b stdout 0

:b stderr 135
type-cast/error-pointers-can-only-be-casted-to-other-pointers-and-u64-integers.glos:3:9: ERROR: Cannot cast type '&i64' to type 'bool'

:b testcase 18
sizeof/basics.glos
:i returncode 0
:b stdout 18
1
8
8
8
8
0
8
1
8

:b stderr 0

:b testcase 18
extern/basics.glos
:i returncode 69
:b stdout 0

:b stderr 2
E

:b testcase 22
type-alias/basics.glos
:i returncode 0
:b stdout 7
69
420

:b stderr 0

