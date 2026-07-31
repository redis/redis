/**
 * @name Potential dynamic lookup of OpenSSL CMS and S/MIME symbols
 * @description Finds relevant symbol string literals, direct dlsym uses of those literals, and dynamic-loader calls for review.
 * @kind problem
 * @problem.severity warning
 * @precision high
 * @id cpp/redis-cve-2026-28390-dynamic-cms-lookup
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

predicate isCmsOrSmimeSymbol(string value) {
  value.regexpMatch("^(CMS_decrypt|CMS_.*|d2i_CMS_.*|i2d_CMS_.*|SMIME_.*)$")
}

predicate evidence(Expr location, string kind, string symbol) {
  exists(StringLiteral literal |
    location = literal and
    isCmsOrSmimeSymbol(literal.getValue()) and
    symbol = literal.getValue() and
    kind = "CMS/S-MIME symbol string literal"
  )
  or
  exists(FunctionCall call, Function target, StringLiteral literal |
    location = call and
    call.getTarget() = target and
    target.hasGlobalName("dlsym") and
    call.getNumberOfArguments() > 1 and
    call.getArgument(1).getUnconverted() = literal and
    isCmsOrSmimeSymbol(literal.getValue()) and
    symbol = literal.getValue() and
    kind = "Direct dlsym lookup of CMS/S-MIME symbol"
  )
  or
  exists(FunctionCall call, Function target |
    location = call and
    call.getTarget() = target and
    (target.hasGlobalName("dlopen") or target.hasGlobalName("dlsym")) and
    symbol = target.getName() and
    kind = "Dynamic-loader API call requiring manual review"
  )
}

from Expr location, string kind, string symbol
where evidence(location, kind, symbol)
select location,
  kind + ": " + symbol + " in " + enclosingFunctionName(location) + " at " +
    location.getLocation().toString() + "."
