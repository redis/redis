The utilities in this directory plot the distribution of SRANDMEMBER to
evaluate how fair it is.

See http://theshfl.com/redis_sets for more information on the topic that lead
to such investigation fix.

showdist.rb -- shows the distribution of the frequency elements are returned.
               The x axis is the number of times elements were returned, and
               the y axis is how many elements were returned with such
               frequency.

showfreq.rb -- shows the frequency each element was returned.
               The x axis is the element number.
               The y axis is the times it was returned.
