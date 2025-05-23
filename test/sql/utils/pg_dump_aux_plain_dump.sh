DUMPFILE=${DUMPFILE:-$1}
EXTRA_PGOPTIONS=${EXTRA_PGOPTIONS:-$2}
DUMP_OPTIONS=${DUMP_OPTIONS:-$3}
# Override PGOPTIONS to remove verbose output
PGOPTIONS="--client-min-messages=warning $EXTRA_PGOPTIONS"

export PGOPTIONS
echo ${DUMP_OPTIONS}
echo $DUMP_OPTIONS
echo $(echo $DUMP_OPTIONS)

${PG_BINDIR}/pg_dump -h ${PGHOST} -U ${TEST_ROLE_SUPERUSER} ${DUMP_OPTIONS} -Fp ${TEST_DBNAME} -f ${DUMPFILE}
# ${PG_BINDIR}/pg_dump -h ${PGHOST} -U ${TEST_ROLE_SUPERUSER} ${DUMP_OPTIONS} -Fp ${TEST_DBNAME} > /dev/null 2>&1 -f ${DUMPFILE}
${PG_BINDIR}/dropdb -h ${PGHOST} -U ${TEST_ROLE_SUPERUSER} ${TEST_DBNAME}
${PG_BINDIR}/createdb -h ${PGHOST} -U ${TEST_ROLE_SUPERUSER} ${TEST_DBNAME}
