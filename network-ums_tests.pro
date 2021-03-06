PROOF_PRI_PATH = $$PWD/../proofboot
!exists($$PROOF_PRI_PATH/proof_tests.pri):PROOF_PRI_PATH = $$(PROOF_PATH)
include($$PROOF_PRI_PATH/proof_tests.pri)

QT += network
CONFIG += proofnetwork proofnetworkums

SOURCES += \
    tests/proofnetwork/ums/main.cpp \
    tests/proofnetwork/ums/umsuser_test.cpp \
    tests/proofnetwork/ums/tokensapi_test.cpp

RESOURCES += \
    tests/proofnetwork/ums/tests_resources.qrc

DISTFILES += \
    tests/proofnetwork/ums/data/umsuser.json \
    tests/proofnetwork/ums/data/umsuser2.json \
    tests/proofnetwork/ums/data/token.json \
    tests/proofnetwork/ums/data/pub_rsa.key
