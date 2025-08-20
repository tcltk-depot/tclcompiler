proc mytest {me} {
    foreach m $me {
        append result $m
    }
    return $result
}
