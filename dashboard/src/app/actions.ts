"use server";

import { revalidatePath } from "next/cache";
import { redirect } from "next/navigation";

import {
  type Operation,
  runOperation,
} from "@/lib/nexusfs";

const allowed = new Set<Operation>([
  "catalog-sync",
  "repair",
  "maintenance",
  "rebalance",
]);

export async function runOperationAction(formData: FormData) {
  const rawOperation = String(formData.get("operation") ?? "");
  let destination = "/?status=error&message=Unknown%20operation";

  if (!allowed.has(rawOperation as Operation)) {
    redirect(destination);
  }

  const operation = rawOperation as Operation;

  try {
    let body: Record<string, unknown> = {};

    if (operation === "rebalance") {
      const epoch = Number(formData.get("membership_epoch"));

      if (!Number.isSafeInteger(epoch) || epoch <= 0) {
        throw new Error(
          "Rebalancing requires a positive membership epoch.",
        );
      }

      body = {
        operation_id: `dashboard-rebalance-${Date.now()}`,
        expected_membership_epoch: epoch,
      };
    }

    await runOperation(operation, body);
    revalidatePath("/");

    destination =
      `/?status=success&message=${encodeURIComponent(
        `${operation} completed successfully`,
      )}`;
  } catch (error: unknown) {
    const message =
      error instanceof Error ? error.message : "Operation failed.";

    destination =
      `/?status=error&message=${encodeURIComponent(message)}`;
  }

  redirect(destination);
}
