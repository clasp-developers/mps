#!/usr/local/bin/perl -w

%Formats = ();

%Types = (
  "P", "void *",
  "U", "unsigned ",
  "D", "double ",
  "A", "Addr ",
);

while(<*.c>) {
  open(C, "<$_") || die "Can't open $_";

  print STDERR "$_ ... ";

  while(<C>) {
    if(/EVENT_([PUAD]+)\(/) { 
      $Formats{$1} = 1 if(!defined($Formats{$1}));
    }
  }

  close(C);
}

print STDERR "\n";

open(H, ">eventgen.h") || die "Can't open eventgen.h for output";

print H "/* impl.h.eventgen -- Automatic event header
 *
 * Copyright (C) 1997 Harlequin Group, all rights reserved.
 * \$HopeName\$
 *
 * !!! DO NOT EDIT THIS FILE !!!
 * This file was generated by eventgen.pl
 */\n\n";

print H "#ifdef EVENT\n\n";

foreach $format ("", sort(keys(%Formats))) {
  print H "typedef struct {\n";
  print H "  Word code;\n  Word length;\n  Word clock;\n";
  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    print H "  ", $Types{$c}, "\l$c$i;\n";
  }
  print H "} Event", $format, "Struct;\n\n";
}

print H "\ntypedef union {\n  EventStruct any;\n";

foreach $format (sort(keys(%Formats))) {
  print H "  Event${format}Struct \L$format;\n";
}
print H "} EventUnion;\n\n\n";


print H "#define EVENT_0(type) \\
  EVENT_BEGIN(type, sizeof(EventStruct)) \\
  EVENT_END(type, sizeof(EventStruct))\n\n";

foreach $format (sort(keys(%Formats))) {
  print H "#define EVENT_$format(type";

  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    print H ", _\l$c$i";
  }

  print H ") \\\n";

  print H "  EVENT_BEGIN(type, sizeof(Event${format}Struct)) \\\n";
  
  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    print H "    Event.\L$format.$c$i = (_$c$i); \\\n";
  }

  print H "  EVENT_END(type, sizeof(Event${format}Struct))\n\n"
}

print H "\n#else /* EVENT not */\n\n";

print H "#define EVENT_0(type)    NOOP\n";

foreach $format (sort(keys(%Formats))) {
  print H "#define EVENT_$format(type";
  for($i = 0; $i < length($format); $i++) {
    print H ", p$i";
  }
  print H ")    NOOP\n";
}

print H "\n#endif /* EVENT */\n";

close(H);
