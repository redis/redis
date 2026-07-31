import cpp

from int compiledSourceFileCount
where
  compiledSourceFileCount =
    count(File file | file.compiledAsC() or file.compiledAsCpp())
select compiledSourceFileCount
