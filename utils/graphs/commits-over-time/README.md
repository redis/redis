This Tcl script is what I used in order to generate the graph you
can find at http://antirez.com/news/98. It's really quick & dirty, more
a trow away program than anything else, but probably could be reused or
modified in the future in order to visualize other similar data or an
updated version of the same data.

The usage is trivial:

    ./genhtml.tcl > output.html

The generated HTML is quite broken but good enough to grab a screenshot
from the browser. Feel free to improve it if you got time / interest.

Note that the code filtering the tags, and the hardcoded branch name, does
not make the script, as it is, able to analyze a different repository.
However the changes needed are trivial.
