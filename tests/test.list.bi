:i count 5
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

