/**
 * @name OpenSSL CMS and S/MIME API calls
 * @description Finds direct calls to OpenSSL CMS, CMS serialization, and S/MIME APIs.
 * @kind problem
 * @problem.severity warning
 * @precision high
 * @id cpp/redis-cve-2026-28390-cms-api-call
 * @tags security
 */

import cpp

predicate isCmsOrSmimeApi(string name) {
  name.regexpMatch("^(CMS_|d2i_CMS_|i2d_CMS_|SMIME_).*$")
}

from FunctionCall call, Function target
where
  call.getTarget() = target and
  isCmsOrSmimeApi(target.getName())
select call,
  "Direct call to OpenSSL CMS/S/MIME API " + target.getQualifiedName() +
    " from " + call.getEnclosingFunction().getQualifiedName() + " at " +
    call.getLocation().toString() + "."
