# Build all .v files listed in _CoqProject.
#
# Inside Docker (from proofs/operational/):
#   eval $(opam env) && make
#
# Outside Docker:
#   docker run --rm -v "$PWD:/work" zer-proofs bash -c 'eval $$(opam env) && make'
#
# Regenerate CoqMakefile after _CoqProject changes:
#   coq_makefile -f _CoqProject -o CoqMakefile

all: CoqMakefile
	$(MAKE) -f CoqMakefile

CoqMakefile: _CoqProject
	coq_makefile -f _CoqProject -o CoqMakefile

clean:
	if [ -f CoqMakefile ]; then $(MAKE) -f CoqMakefile clean; fi
	rm -f CoqMakefile CoqMakefile.conf
	find . -name '*.vo' -delete
	find . -name '*.vok' -delete
	find . -name '*.vos' -delete
	find . -name '*.glob' -delete
	find . -name '.*.aux' -delete

.PHONY: all clean
