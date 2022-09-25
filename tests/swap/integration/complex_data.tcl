start_server {} {
    test {complex data} {
        for {set i 0} {$i < 10} {incr i} {
            createComplexDataset r 1000
            puts "save $i"
            r save
        }
    }
}
