:i count 12
:b testcase 20
integers/basics.glos
:i returncode 0
:b stdout 17
69
420
69
420
69

:b stderr 0

:b testcase 20
booleans/basics.glos
:i returncode 0
:b stdout 4
1
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

