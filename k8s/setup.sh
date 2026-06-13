#!/usr/bin/env bash
# ==============================================================================
# setup.sh — End-to-end cluster setup for HFT Backtesting Engine
# ==============================================================================
# This script:
#   1. Installs kind (Kubernetes IN Docker) if not present
#   2. Creates a 5-node kind cluster (1 control-plane + 4 workers)
#   3. Builds the HFT engine Docker image
#   4. Loads the image into the kind cluster (no registry needed)
#   5. Installs the Kubeflow MPI Operator via kubectl
#   6. Deploys the MPIJob workload
#   7. Watches pod status
#
# Prerequisites:
#   - Docker must be running
#   - kubectl must be installed
#   - Internet access for downloading kind and MPI Operator manifests
#
# Usage:
#   chmod +x k8s/setup.sh
#   ./k8s/setup.sh
# ==============================================================================

set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────
CLUSTER_NAME="hft-cluster"
IMAGE_NAME="hft-engine:latest"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KIND_CONFIG="${SCRIPT_DIR}/kind-config.yaml"
MPIJOB_MANIFEST="${SCRIPT_DIR}/mpijob.yaml"

# MPI Operator version — pin to a specific release for reproducibility.
MPI_OPERATOR_VERSION="v0.5.0"
MPI_OPERATOR_URL="https://raw.githubusercontent.com/kubeflow/mpi-operator/${MPI_OPERATOR_VERSION}/deploy/v2beta1/mpi-operator.yaml"

echo "============================================================"
echo " HFT Backtesting Engine — Kubernetes Setup"
echo "============================================================"

# ──────────────────────────────────────────────────────────────────────────────
# Step 1: Install kind if not already present
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "[1/6] Checking for kind..."

if ! command -v kind &>/dev/null; then
    echo "  → Installing kind..."
    # Download the kind binary for Linux amd64.
    curl -Lo /usr/local/bin/kind \
        "https://kind.sigs.k8s.io/dl/v0.23.0/kind-linux-amd64"
    chmod +x /usr/local/bin/kind
    echo "  ✓ kind installed at $(which kind)"
else
    echo "  ✓ kind already installed: $(kind version)"
fi

# ──────────────────────────────────────────────────────────────────────────────
# Step 2: Create the kind cluster
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "[2/6] Creating kind cluster '${CLUSTER_NAME}'..."

if kind get clusters 2>/dev/null | grep -q "^${CLUSTER_NAME}$"; then
    echo "  ✓ Cluster '${CLUSTER_NAME}' already exists — skipping creation."
else
    kind create cluster \
        --name "${CLUSTER_NAME}" \
        --config "${KIND_CONFIG}" \
        --wait 120s
    echo "  ✓ Cluster created."
fi

# Ensure kubectl context is set to the new cluster.
kubectl cluster-info --context "kind-${CLUSTER_NAME}"
echo ""

# ──────────────────────────────────────────────────────────────────────────────
# Step 3: Build the Docker image
# ──────────────────────────────────────────────────────────────────────────────
echo "[3/6] Building Docker image '${IMAGE_NAME}'..."

docker build -t "${IMAGE_NAME}" "${PROJECT_ROOT}"
echo "  ✓ Image built."

# ──────────────────────────────────────────────────────────────────────────────
# Step 4: Load the image into the kind cluster
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "[4/6] Loading image into kind cluster..."

# kind load copies the image from the local Docker daemon into all cluster
# nodes, eliminating the need for a container registry.
kind load docker-image "${IMAGE_NAME}" --name "${CLUSTER_NAME}"
echo "  ✓ Image loaded into cluster."

# ──────────────────────────────────────────────────────────────────────────────
# Step 5: Install the Kubeflow MPI Operator
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "[5/6] Installing MPI Operator ${MPI_OPERATOR_VERSION}..."

# Apply the MPI Operator CRDs and controller.  This is idempotent — re-running
# will update existing resources without error.
kubectl apply -f "${MPI_OPERATOR_URL}"

# Wait for the MPI Operator controller to become ready.
echo "  → Waiting for MPI Operator deployment to be ready..."
kubectl wait --for=condition=available \
    deployment/mpi-operator \
    -n mpi-operator \
    --timeout=120s 2>/dev/null || \
    echo "  ⚠ Could not verify MPI Operator readiness (namespace may differ)."

echo "  ✓ MPI Operator installed."

# ──────────────────────────────────────────────────────────────────────────────
# Step 6: Deploy the MPIJob
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "[6/6] Deploying MPIJob..."

kubectl apply -f "${MPIJOB_MANIFEST}"
echo "  ✓ MPIJob submitted."

# ──────────────────────────────────────────────────────────────────────────────
# Watch pod status
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "============================================================"
echo " Setup complete!  Watching pod status..."
echo " Press Ctrl+C to stop watching."
echo "============================================================"
echo ""

kubectl get pods -l app=hft-engine --watch
