:i count 10
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

:b testcase 23
003-variables/main.glos
:i returncode 0
:b stdout 18
69
420
1337
80085

:b stderr 0

:b testcase 34
003-variables/error-undefined.glos
:i returncode 1
:b stdout 0

:b stderr 75
003-variables/error-undefined.glos:2:11: ERROR: Undefined identifier 'foo'

:b testcase 37
003-variables/error-redefinition.glos
:i returncode 1
:b stdout 0

:b stderr 143
003-variables/error-redefinition.glos:2:5: ERROR: Redefinition of identifier 'x'
003-variables/error-redefinition.glos:1:5: NOTE: Defined here

:b testcase 60
003-variables/error-assignment-definition-type-mismatch.glos
:i returncode 1
:b stdout 0

:b stderr 106
003-variables/error-assignment-definition-type-mismatch.glos:1:13: ERROR: Expected type 'i64', got 'bool'

:b testcase 48
003-variables/error-undefined-outside-scope.glos
:i returncode 1
:b stdout 0

:b stderr 87
003-variables/error-undefined-outside-scope.glos:7:11: ERROR: Undefined identifier 'x'

:b testcase 67
003-variables/error-undefined-outside-scope-despite-same-depth.glos
:i returncode 1
:b stdout 0

:b stderr 106
003-variables/error-undefined-outside-scope-despite-same-depth.glos:6:15: ERROR: Undefined identifier 'x'

:b testcase 23
004-functions/main.glos
:i returncode 0
:b stdout 21
69
420
69
420
69
420

:b stderr 0

