proc mytest {me} {
    switch $me {
        1 {return 1}
        default {return $me}
    }
}
