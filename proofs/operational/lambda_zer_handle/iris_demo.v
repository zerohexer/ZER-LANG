(* ================================================================
   Phase 0c — Minimal wp proof using the λZER-Handle language instance.

   Proves the simplest non-trivial Iris fact about our language:
   `WP (EVal v) {{ r, r = v }}` — a value evaluates to itself.

   This tests:
     - The language instance is usable by Iris wp.
     - `iApply wp_value` works on our types.
     - Iris's proof mode (`iIntros`, `iApply`) plugs in cleanly.

   Phase 1 will extend this with:
     - A resource algebra for `alive_handle p i g : iProp`.
     - wp specs for pool.alloc, pool.free, pool.get (each consumes/
       produces the resource appropriately — safety encoded as
       separation logic resource ownership).
     - The full alloc→free program proof, connected via Iris
       adequacy to operational safety (no stuck non-values).

   For Phase 0, the trivial state interpretation (`state_interp := True`)
   is sufficient. Phase 1 replaces it with a real heap invariant.
   ================================================================ *)

From iris.proofmode Require Import proofmode.
From iris.program_logic Require Import weakestpre.
From zer_handle Require Import syntax iris_lang.

Section demo.
  Context `{!irisGS_gen HasNoLc λZH_lang Σ}.

  (* Values evaluate to themselves — the simplest wp fact.
     Note: we qualify `syntax.val` because `val` alone resolves
     to Iris's `language → Type` projection after `weakestpre`
     is imported. Our concrete `expr` and `EVal` constructor
     unify with Iris's projection types via the Canonical Structure
     declared in iris_lang.v. *)
  Lemma wp_val (v : syntax.val) :
    ⊢ WP (EVal v) {{ r, ⌜r = v⌝ }}.
  Proof.
    iApply wp_value. iPureIntro. reflexivity.
  Qed.

End demo.
