# Проверка исправления после Failed от бота

Бот получил пустой список итераций для 9 детей. Удалено перенаправление
stderr в stdout перед print(): библиотека должна сохранять штатный поток вывода.
Старые тесты ожидали ошибочно перенаправленный вывод и потому пропустили дефект.
Новый тест на старом бинарнике с 9 детьми завершился AssertionError.
Вложение pa4_results.tar.gz.gpg в рабочей папке отсутствует; механизм ошибки
установлен по тексту ответа бота, реализации print и локальному воспроизведению.

Сборка: локальный Docker-образ `pa3-legacy:local`, clang 3.5.

```
clang-3.5 -std=c99 -Wall -Wextra -Werror -pedantic main.c ipc.c clock.c mutex.c protocol.c -L. -lruntime -o pa4
```

Код возврата 0, предупреждений нет. Полученный бинарник проверен с оригинальной
библиотекой из `../lab3/pa3/libruntime.so` в образе `pa3-toolchain:local`.
Библиотека также указана в LD_PRELOAD. Проверки ниже выполнены после удаления dup2.

```
OK: p=1 mutex=False
OK: p=1 mutex=True
OK: p=2 mutex=False
OK: p=2 mutex=True
OK: p=5 mutex=False
OK: p=5 mutex=True
OK: p=9 mutex=False
OK: p=9 mutex=True
OK: p=15 mutex=False
OK: p=15 mutex=True
OK: p=9 mutex=True silent=True
```

Проверены код 0, полнота печати, барьеры, количество каналов и отсутствие
пересечений критических секций. В обычном режиме stderr содержит только итерации
библиотеки, stdout — только события жизненного цикла. В PA45_SILENT=1 stderr пустой.

Дополнительно tests/check_archive.py распаковал обновлённый pa4.tar.gz,
собрал его clang 14 с флагами из PDF и запустил 9 детей с --mutexl без --trace:

```
PASS: extracted archive, PDF compiler flags, 9 workers, 225 exact iteration lines, no --trace
```

Полного окружения бота локально нет. Повторный Passed ещё не получен.

## Дополнение: режим --stats

После добавления stats.c сборка clang 3.5 с -Wall -Wextra -Werror -pedantic
прошла без предупреждений. Все 11 интеграционных сценариев повторены с --stats:
проверены точные числа сообщений каждого типа для каждого процесса и совпадение
суммарного/максимального ожидания с трассой REQUEST/ENTER.

```
All 11 integration cases passed, including message counts and wait statistics.
```

Пересобранный архив повторно распакован, собран и проверен на 9 процессах
без --stats и --trace: получены все 225 строк, дополнительные файлы не создаются.
Все 11 файлов внутри архива побайтово совпадают с рабочими исходниками и Makefile.
