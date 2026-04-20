# Coq + Iris build environment for ZER-LANG operational-semantics proofs.
#
# Build: docker build -t zer-proofs -f proofs/operational/Dockerfile .
# Run:   docker run --rm -v "$PWD/proofs/operational:/work" zer-proofs make
#
# Uses pre-built coqorg/coq:8.18 image (saves 20+ minutes of opam compile
# vs building Coq from scratch). Iris + stdpp are opam-installed on top
# and opam resolves compatible versions automatically — better than
# pinning versions that drift out of the opam repository.

FROM coqorg/coq:8.18

USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
    make \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

USER coq

# Install Iris and stdpp via opam. The coqorg/coq image already has
# opam + coq-released repo configured. Letting opam pick compatible
# versions avoids version-drift errors.
RUN opam update -y && \
    opam install -y coq-iris coq-stdpp

# Verify installation.
RUN eval $(opam env) && \
    coqc --version && \
    echo "--- Iris check ---" && \
    echo 'From iris.base_logic Require Import iprop.' | coqtop -batch 2>&1 | head -5

WORKDIR /work

CMD ["bash", "-c", "eval $(opam env) && ls -la"]
