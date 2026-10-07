#!/usr/bin/env python3
"""Compile the actual lesson scheduling adapters against the host boundary."""
from pathlib import Path
import sys

# application.cc is split into application_*.cc translation units; search them all.
application = Path(sys.argv[1])
source = '\n'.join(path.read_text() for path in
                   [application, *sorted(application.parent.glob('application_*.cc'))])
methods = []
for signature in (
    'bool Application::IsChatRequestCurrent',
    'bool Application::IsChatLessonRequestCurrent',
    'void Application::ScheduleChatLesson',
):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    methods.append(source[start:end])
Path(sys.argv[2]).write_text('#include "application.h"\n' + '\n\n'.join(methods) + '\n')
