# Initialization tests -- most units will start including this.

test "(init) Restart killed instances" {
    foreach type {redis} {
        foreach_${type}_id id {
            if {[get_instance_attrib $type $id pid] == -1} {
                puts -nonewline "$type/$id "
                flush stdout
                restart_instance $type $id
            }
        }
    }
}
