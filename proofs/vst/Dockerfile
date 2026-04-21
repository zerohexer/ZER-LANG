# Coq + VST build environment for ZER-LANG Level-3 compiler verification.
#
# Adds VST (Verified Software Toolchain) on top of the operational-proofs
# Docker image. VST lets us verify C source code (e.g., zercheck.c
# functions) against Coq specifications.
#
# Build: docker build -t zer-vst -f proofs/vst/Dockerfile .
# Run:   docker run --rm -v "$PWD/proofs/vst:/work" zer-vst make
#
# VST install takes ~30-60 minutes on first build (compiles from source).
# Subsequent rebuilds are cached by Docker.

FROM coqorg/coq:8.18

USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
    make \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

USER coq

# Install Iris + stdpp first (needed for other proof work alongside VST).
RUN opam update -y && \
    opam install -y coq-iris coq-stdpp

# Install VST. `coq-vst` is the main library; `coq-vst-zlist` gives a
# powerful automatic list solver. VST takes a while to compile.
RUN opam install -y coq-vst coq-vst-zlist

# Precompile VST.floyd.compat — provides the backward-compatible
# funspec without Σ parameter (for proofs that don't need custom
# ghost state). Not compiled by default in the opam package.
RUN eval $(opam env) && \
    cd /home/coq/.opam/4.13.1+flambda/lib/coq/user-contrib/VST/floyd/ && \
    coqc -Q /home/coq/.opam/4.13.1+flambda/lib/coq/user-contrib/VST VST compat.v

# Verify VST installation.
RUN eval $(opam env) && \
    coqc --version && \
    echo "--- VST check ---" && \
    echo 'From VST.floyd Require Import proofauto compat.' | coqtop -batch 2>&1 | head -10

WORKDIR /work

CMD ["bash", "-c", "eval $(opam env) && ls -la"]
