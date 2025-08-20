namespace eval demo {
    variable x 42
    proc getx {} { variable x; return $x }
}
