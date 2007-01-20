files=$(wildcard *.test)
goal=$(patsubst %.test,%.ok,$(files))

all: compile $(goal) report

compile:
	make -C ../src
	g++ -Wall -g -o client client.cc

report: $(goal)
	@echo ""
	@./report.pl $(goal)

clean:
	@rm -rf tmp/ *.o *.bin *.ok *.log *~ DEADJOE

.SUFFIXES: .test .cc .bin .ok

.test.ok:
	@echo ""
	@echo run $*.test
	@./$*.test ; echo $$? > $*.ok
