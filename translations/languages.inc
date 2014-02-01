TRANS_LANGS  = ru
TRANS_TARGET = $$member(TARGET,0)
TRANSLATIONS = \
               $${TRANS_SOURCE_ROOT}/translations/en/$${TRANS_TARGET}.ts \
               $${TRANS_SOURCE_ROOT}/translations/ru/$${TRANS_TARGET}.ts
include(../qmake/translations.inc)
