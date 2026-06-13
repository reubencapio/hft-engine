#!/usr/bin/env bash
# ==============================================================================
# entrypoint.sh — Kubernetes pod entrypoint for HFT Engine
# ==============================================================================
# Dispatches based on MPI_ROLE:
#   launcher  — Runs mpirun to distribute work across worker pods.
#   worker    — Starts sshd and waits, acting as an MPI execution target.
#
# The MPI Operator injects the hostfile at /etc/mpi/hostfile automatically.
# ==============================================================================

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Start the SSH daemon in the background.
# Both launcher and worker pods need sshd so that MPI can establish
# connections in either direction.
# ─────────────────────────────────────────────────────────────────────────────
echo "[entrypoint] Starting sshd..."
/usr/sbin/sshd

if [ "${MPI_ROLE:-}" = "launcher" ]; then
    # ─────────────────────────────────────────────────────────────────────
    # LAUNCHER MODE
    # ─────────────────────────────────────────────────────────────────────
    # The MPI Operator mounts the worker hostfile at a well-known path.
    # We pass it to mpirun so it knows which pods to SSH into.
    # ─────────────────────────────────────────────────────────────────────
    HOSTFILE="${OMPI_MCA_orte_default_hostfile:-/etc/mpi/hostfile}"

    echo "[entrypoint] MPI_ROLE=launcher"
    echo "[entrypoint] Hostfile: ${HOSTFILE}"

    if [ -f "${HOSTFILE}" ]; then
        echo "[entrypoint] Hostfile contents:"
        cat "${HOSTFILE}"
    else
        echo "[entrypoint] WARNING: Hostfile not found at ${HOSTFILE}"
    fi

    echo "[entrypoint] Launching mpirun..."

    # --allow-run-as-root:  Required when running as root inside containers.
    # --hostfile:           Worker pod list injected by the MPI Operator.
    # --map-by slot:        Map MPI ranks to slots on each worker.
    # --bind-to core:       Pin each rank to a specific core for consistency.
    # -x OMP_NUM_THREADS:   Forward the OpenMP thread count to all ranks.
    # -x OMP_PROC_BIND:     Forward the OpenMP binding policy.
    # -x OMP_PLACES:        Forward the OpenMP placement policy.
    exec mpirun \
        --allow-run-as-root \
        --hostfile "${HOSTFILE}" \
        --map-by slot \
        --bind-to core \
        -x OMP_NUM_THREADS \
        -x OMP_PROC_BIND \
        -x OMP_PLACES \
        /usr/local/bin/hft-engine "$@"

elif [ "${MPI_ROLE:-}" = "worker" ]; then
    # ─────────────────────────────────────────────────────────────────────
    # WORKER MODE
    # ─────────────────────────────────────────────────────────────────────
    # Workers simply keep sshd running so the launcher can connect.
    # We wait indefinitely; the pod is terminated by Kubernetes when the
    # MPIJob completes.
    # ─────────────────────────────────────────────────────────────────────
    echo "[entrypoint] MPI_ROLE=worker — sshd running, waiting for connections..."
    exec tail -f /dev/null

else
    # ─────────────────────────────────────────────────────────────────────
    # STANDALONE / DEFAULT MODE
    # ─────────────────────────────────────────────────────────────────────
    # If MPI_ROLE is not set, run the engine directly (useful for local
    # testing outside of Kubernetes).
    # ─────────────────────────────────────────────────────────────────────
    echo "[entrypoint] MPI_ROLE not set — running standalone..."
    exec /usr/local/bin/hft-engine "$@"
fi
