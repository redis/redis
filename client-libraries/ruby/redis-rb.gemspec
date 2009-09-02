# -*- encoding: utf-8 -*-

Gem::Specification.new do |s|
  s.name = %q{redis}
  s.version = "0.1"

  s.required_rubygems_version = Gem::Requirement.new(">= 0") if s.respond_to? :required_rubygems_version=
  s.authors = ["Ezra Zygmuntowicz", "Taylor Weibley", "Matthew Clark", "Brian McKinney", "Salvatore Sanfilippo", "Luca Guidi"]
  # s.autorequire = %q{redis-rb}
  s.date = %q{2009-06-23}
  s.description = %q{Ruby client library for redis key value storage server}
  s.email = %q{ez@engineyard.com}
  s.extra_rdoc_files = ["LICENSE"]
  s.files = ["LICENSE", "README.markdown", "Rakefile", "lib/dist_redis.rb", "lib/hash_ring.rb", "lib/pipeline.rb", "lib/redis.rb", "spec/redis_spec.rb", "spec/spec_helper.rb"]
  s.has_rdoc = true
  s.homepage = %q{http://github.com/ezmobius/redis-rb}
  s.require_paths = ["lib"]
  s.rubygems_version = %q{1.3.1}
  s.summary = %q{Ruby client library for redis key value storage server}

  if s.respond_to? :specification_version then
    current_version = Gem::Specification::CURRENT_SPECIFICATION_VERSION
    s.specification_version = 2

    if Gem::Version.new(Gem::RubyGemsVersion) >= Gem::Version.new('1.2.0') then
    else
    end
  else
  end
end
