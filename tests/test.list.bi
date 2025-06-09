:i count 3
:b testcase 22
001-integers/main.glos
:i returncode 0
:b stdout 17
69
420
69
420
69

:b stderr 0

:b testcase 24
002-conditions/main.glos
:i returncode 0
:b stdout 18
69
420
1337
80085

:b stderr 0

:b testcase 54
002-conditions/error-expected-condition-type-bool.glos
:i returncode 1
:b stdout 0

:b stderr 99
002-conditions/error-expected-condition-type-bool.glos:2:8: ERROR: Expected type 'bool', got 'i64'

