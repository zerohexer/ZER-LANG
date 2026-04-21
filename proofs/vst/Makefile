# Build all VST proofs.
#
# Build the Docker image once: docker build -t zer-vst -f proofs/vst/Dockerfile proofs/vst
# Then from the repo root: make check-vst

.PHONY: all clean

# Default target: generate Clight AST + compile Coq proofs.
all: simple_check.vo verif_simple_check.vo

# Regenerate CompCert Clight AST from C source.
simple_check.v: simple_check.c
	clightgen -normalize simple_check.c

simple_check.vo: simple_check.v
	coqc -Q . zer_vst simple_check.v

verif_simple_check.vo: verif_simple_check.v simple_check.vo
	coqc -Q . zer_vst verif_simple_check.v

clean:
	rm -f *.vo *.vos *.vok *.glob simple_check.v *.aux
