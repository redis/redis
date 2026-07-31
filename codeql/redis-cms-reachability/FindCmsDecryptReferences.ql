/**
 * @name References to OpenSSL CMS_decrypt
 * @description Finds non-call function accesses to CMS_decrypt, including uses as a function pointer value.
 * @kind problem
 * @problem.severity warning
 * @precision very-high
 * @id cpp/redis-cve-2026-28390-cms-decrypt-reference
 * @tags security
 */

import cpp

string enclosingFunctionName(Expr expression) {
  exists(Function function |
    function = expression.getEnclosingFunction() and
    result = function.getQualifiedName()
  )
  or
  not exists(expression.getEnclosingFunction()) and result = "<global scope>"
}

/*
 * FunctionAccess excludes the ordinary callee expression of a FunctionCall.
 * It therefore complements FindCmsDecryptCalls and detects statically resolved
 * uses such as "&CMS_decrypt" or assignment to a function pointer. It does not
 * prove where an arbitrarily reassigned function pointer is later invoked.
 */
from FunctionAccess access, Function target
where
  access.getTarget() = target and
  target.hasGlobalName("CMS_decrypt")
select access,
  "Non-call reference to OpenSSL CMS_decrypt from " +
    enclosingFunctionName(access) + " at " +
    access.getLocation().toString() + "; resolved target: " +
    target.getQualifiedName() + "."
