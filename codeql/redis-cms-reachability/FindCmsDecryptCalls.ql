/**
 * @name Direct calls to OpenSSL CMS_decrypt
 * @description Finds statically resolved direct calls to the global OpenSSL CMS_decrypt function.
 * @kind problem
 * @problem.severity warning
 * @precision very-high
 * @id cpp/redis-cve-2026-28390-cms-decrypt-call
 * @tags security
 */

import cpp

from FunctionCall call, Function target
where
  call.getTarget() = target and
  target.hasGlobalName("CMS_decrypt")
select call,
  "Direct call to OpenSSL CMS_decrypt from " +
    call.getEnclosingFunction().getQualifiedName() + " at " +
    call.getLocation().toString() + "; resolved target: " +
    target.getQualifiedName() + "."
