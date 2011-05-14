-- Script to find the install path for a C module. Public domain.

if not arg or not arg[1] then
  io.write("Usage: lua installpath.lua modulename\n")
  os.exit(1)
end
for p in string.gfind(package.cpath, "[^;]+") do
  if string.sub(p, 1, 1) ~= "." then
    local p2 = string.gsub(arg[1], "%.", string.sub(package.config, 1, 1))
    io.write(string.gsub(p, "%?", p2), "\n")
    return
  end
end
error("no suitable installation path found")
