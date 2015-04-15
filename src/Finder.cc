/*
 * Rados Filesystem - A filesystem library based in librados
 *
 * Copyright (C) 2014 CERN, Switzerland
 *
 * Author: Joaquim Rocha <joaquim.rocha@cern.ch>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License at http://www.gnu.org/licenses/lgpl-3.0.txt
 * for more details.
 */

#include <errno.h>
#include <regex.h>
#include <sys/stat.h>
#include <utility>

#include "radosfscommon.h"
#include "Finder.hh"
#include "Logger.hh"

#define DEFAULT_NUM_FINDER_THREADS 100
#define MAX_INACTIVE_THREAD_TIME 5

RADOS_FS_BEGIN_NAMESPACE

Finder::Finder(Filesystem *fs)
  : radosFs(fs)
{}

Finder::~Finder()
{}

static int
runRegex(const std::string &expression, regex_t regex, const std::string &entry)
{
  int ret = regexec(&regex, entry.c_str(), 0, 0, 0);

  if (ret != 0)
  {
    if (ret != REG_NOMATCH)
      radosfs_debug("Error looking for regex: %s", expression.c_str());
  }

  return ret;
}

static int
makeExpRegex(const std::string &exp, bool icase, regex_t *regex)
{
  int ret = 0;
  int flags = 0;

  if (exp == "")
    return -EINVAL;

  if (icase)
    flags = REG_ICASE;

  ret = regcomp(regex, exp.c_str(), flags);

  return ret;
}

static int
checkEntryRegex(const std::string &value, const std::string &valueToCompare,
                Finder::FindOptions option, regex_t regex)
{
  int ret = runRegex(value, regex, valueToCompare);

  regfree(&regex);

  if (((option & Finder::FIND_EQ) && ret == 0) ||
      ((option & Finder::FIND_NE) && ret == REG_NOMATCH))
  {
    return 0;
  }

  return -1;
}

int
Finder::checkEntrySize(FinderArg &arg, FindOptions option,
                       const std::string &entry, const Dir &dir,
                       struct stat &buff)
{
  int ret = 0;

  if (buff.st_nlink == 0)
  {
    ret = radosFs->stat(dir.path() + entry, &buff);

    if (ret != 0)
    {
      radosfs_debug("Error stating %s", (dir.path() + entry).c_str());
      return ret;
    }
  }

  return compareEntryNumValue(arg, option, buff.st_size);
}

int
Finder::checkEntryMtd(FinderArg &arg, FindOptions option,
                      const std::string &entry, Dir &dir)
{
  int ret = 0;
  std::string mtdValue;
  bool checkingName = arg.key.empty();
  std::string key = checkingName ? arg.valueStr : arg.key;

  ret = dir.getMetadata(entry, key, mtdValue);

  if (ret == 0)
  {
    regex_t regex;
    bool icase = arg.options & FinderArg::FINDER_OPT_CMP_NUM;
    ret = makeExpRegex(arg.valueStr, icase, &regex);

    if (ret != 0)
    {
      radosfs_debug("Error making regex for %s on directory '%s'.",
                    arg.valueStr.c_str(), dir.path().c_str());
      return -EINVAL;
    }

    if (checkingName)
      ret = checkEntryRegex(arg.valueStr, key, option, regex);
    else
      ret = checkEntryRegex(arg.valueStr, mtdValue, option, regex);
  }
  else if (checkingName && option == FIND_MTD_NE)
  {
    ret = 0;
  }

  return ret;
}

int
Finder::checkXAttrKeyPresence(FinderArg &arg, FindOptions option,
                              const std::map<std::string, std::string> &xattrs)
{
  regex_t regex;
  bool icase = arg.options & FinderArg::FINDER_OPT_ICASE;
  int ret = makeExpRegex(arg.valueStr, icase, &regex);

  if (ret != 0)
  {
    radosfs_debug("Error making regex for %s.",
                  arg.valueStr.c_str());
    return -EINVAL;
  }

  bool matched = false;

  std::map<std::string, std::string>::const_iterator it;
  for (it = xattrs.begin(); it != xattrs.end(); it++)
  {
    ret = runRegex(arg.valueStr, regex, (*it).first);
    if (ret == 0)
    {
      matched = true;
      break;
    }
  }

  if ((matched && option == FIND_XATTR_EQ) ||
      (!matched && option == FIND_XATTR_NE))
    return 0;

  return -1;
}

int
Finder::compareEntryStrValue(FinderArg &arg, const std::string &entry,
                             FindOptions option, const std::string &value,
                             Dir &dir)
{
  regex_t regex;
  bool icase = arg.options & FinderArg::FINDER_OPT_ICASE;
  int ret = makeExpRegex(arg.valueStr, icase, &regex);

  if (ret != 0)
  {
    radosfs_debug("Error making regex for %s on directory '%s' for entry '%s'.",
                  arg.valueStr.c_str(), dir.path().c_str(),
                  entry.c_str());
    return -EINVAL;
  }

  return checkEntryRegex(arg.valueStr, value, option, regex);
}

int
Finder::compareEntryNumValue(FinderArg &arg, FindOptions option, float value)
{
  int ret = -1;

  if ((option & FIND_EQ) && (value == arg.valueNum))
  {
      ret = 0;
  }
  else if ((option & FIND_NE) && (value != arg.valueNum))
  {
    ret = 0;
  }
  else if ((option & FIND_LT) && (value < arg.valueNum))
  {
    ret = 0;
  }
  else if ((option & FIND_GT) && (value > arg.valueNum))
  {
    ret = 0;
  }

  return ret;
}

int
Finder::checkEntryXAttrs(FinderArg &arg, FindOptions option,
                         const std::string &entry, Dir &dir)
{
  int ret = 0;
  std::string mtdValue;
  bool checkingName = arg.key.empty();
  std::string key = checkingName ? arg.valueStr : arg.key;
  std::map<std::string, std::string> xattrs;

  if (checkingName)
  {
    if ((option != FIND_XATTR_EQ) && (option != FIND_XATTR_NE))
    {
      ret = -EINVAL;
    }
    else
    {
      radosFs->getXAttrsMap(dir.path() + entry, xattrs);
      ret = checkXAttrKeyPresence(arg, option, xattrs);
    }
  }
  else
  {
    ret = radosFs->getXAttr(dir.path() + entry, key, mtdValue);

    if (ret >= 0)
    {
      if (arg.options & FinderArg::FINDER_OPT_CMP_NUM)
      {
        // The value will be 0 if the xattribute value is empty. Perhaps this
        // behavior might not fit every use case so it might need to be
        // changed in the future.

        float value = atof(mtdValue.c_str());
        ret = compareEntryNumValue(arg, option, value);
      }
      else
      {
        ret = compareEntryStrValue(arg, entry, option, mtdValue, dir);
      }
    }
  }

  return ret;
}

int
Finder::checkEntryName(FinderArg &arg, FindOptions option,
                       const std::string &entry)
{
  regex_t regex;
  bool icase = arg.options & FinderArg::FINDER_OPT_ICASE;
  int ret = makeExpRegex(arg.valueStr, icase, &regex);

  if (ret == 0)
  {
    ret = checkEntryRegex(arg.valueStr, entry, option, regex);
  }

  return ret;
}

int
Finder::find(FinderData *data)
{
  int ret;
  std::set<std::string> entries;
  Dir dir(radosFs, data->dir);
  std::set<std::string>::iterator it;

  dir.update();

  if (dir.isLink())
    return 0;

  ret = dir.entryList(entries);

  for (it = entries.begin(); it != entries.end(); it++)
  {
    struct stat buff;
    buff.st_nlink = 0;

    const std::string &entry = *it;

    bool isDir = isDirPath(entry);

    if (isDir)
      data->dirEntries.insert(dir.path() + entry);

    std::map<Finder::FindOptions, FinderArg>::const_iterator it;
    for (it = data->args->begin(); it != data->args->end(); it++)
    {
      Finder::FindOptions option = (*it).first;
      FinderArg arg = (*it).second;

      if (option & FIND_NAME)
      {
        ret = checkEntryName(arg, option, entry);
      }
      if (option & FIND_SIZE)
      {
        ret = checkEntrySize(arg, option, entry, dir, buff);
      }
      else if (option & FIND_MTD)
      {
        ret = checkEntryMtd(arg, option, entry, dir);
      }
      else if (option & FIND_XATTR)
      {
        ret = checkEntryXAttrs(arg, option, entry, dir);
      }

      if (ret != 0)
        break;
    }

    if (ret != 0)
    {
      ret = 0;
      continue;
    }

    data->results.insert(dir.path() + entry);
  }

  return ret;
}

RADOS_FS_END_NAMESPACE
