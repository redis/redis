##
# LocalJumpError ISO Test

assert('LocalJumoError', '15.2.25') do
  begin
    # this will cause an exception due to the wrong location
    retry
  rescue => e1
  end
  LocalJumpError.class == Class and e1.class == LocalJumpError
end

# TODO 15.2.25.2.1 LocalJumpError#exit_value
# TODO 15.2.25.2.2 LocalJumpError#reason
