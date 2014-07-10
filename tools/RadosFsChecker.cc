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

#include <cstdio>
#include <cstdlib>
#include <rados/librados.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "radosfscommon.h"
#include "radosfsdefines.h"
#include "RadosFsChecker.hh"
#include "RadosFsPriv.hh"

static int
getObjectsFromCluster(RadosFsPool *pool, const std::string &prefix,
                      std::map<std::string, std::string> &entries)
{
  int ret;
  rados_list_ctx_t list;

  if ((ret = rados_objects_list_open(pool->ioctx, &list)) != 0)
    return ret;

  const char *obj, *c;
  while (rados_objects_list_next(list, &obj, &c) == 0 && obj != 0)
  {
    if (prefix != obj && !nameIsStripe(obj))
      entries[obj] = pool->name;
  }

  rados_objects_list_close(list);

  return 0;
}

RadosFsChecker::RadosFsChecker(radosfs::RadosFs *radosFs)
  : mRadosFs(radosFs)
{}

bool
RadosFsChecker::checkPath(const std::string &path)
{
  RadosFsStat stat;
  int ret = mRadosFs->mPriv->stat(path, &stat);

  if (ret != 0)
  {
    if (path[path.length() - 1] == PATH_SEP)
      mBrokenDirs[stat.path] = "";
    else
      mBrokenFiles.insert(stat.path);

    return false;
  }

  struct stat statBuff;
  if (S_ISLNK(stat.statBuff.st_mode))
  {
    if (mRadosFs->stat(stat.translatedPath, &statBuff) != 0)
      mBrokenLinks[stat.path] = stat.translatedPath;
  }
  else if (S_ISDIR(stat.statBuff.st_mode))
  {
    mDirs.erase(stat.path);
    return true;
  }
  else if (S_ISREG(stat.statBuff.st_mode))
  {
    if (mInodes.erase(stat.translatedPath) == 0)
    {
      mBrokenFiles.insert(stat.path);
    }
  }

  return false;
}

void
RadosFsChecker::checkDirRecursive(const std::string &path)
{
  radosfs::RadosFsDir dir(mRadosFs, path);

  if (!dir.exists())
  {
    mBrokenDirs[dir.path()] = "";
    return;
  }

  dir.update();

  std::set<std::string> entries;

  dir.entryList(entries);

  std::set<std::string>::const_iterator it;
  for (it = entries.begin(); it != entries.end(); it++)
  {
    const std::string &entry = dir.path() + *it;
    if (checkPath(entry))
      checkDirRecursive(entry);
  }
}

int
RadosFsChecker::check()
{
  radosfs::RadosFsPoolMap::const_iterator mtdMapIt;
  radosfs::RadosFsPoolListMap::const_iterator dataMapIt;
  std::set<std::string> prefixes;

  fprintf(stdout, "Checking...\n");

  for (mtdMapIt = mRadosFs->mPriv->mtdPoolMap.begin();
       mtdMapIt != mRadosFs->mPriv->mtdPoolMap.end();
       mtdMapIt++)
  {
    int ret;

    ret = getObjectsFromCluster((*mtdMapIt).second.get(), (*mtdMapIt).first,
                                mDirs);

    if (ret != 0)
      return ret;

    prefixes.insert((*mtdMapIt).first);
  }

  for (dataMapIt = mRadosFs->mPriv->poolMap.begin();
       dataMapIt != mRadosFs->mPriv->poolMap.end();
       dataMapIt++)
  {
    int ret;

    const radosfs::RadosFsPoolList &pools = (*dataMapIt).second;
    radosfs::RadosFsPoolList::const_iterator poolIt;

    for (poolIt = pools.begin(); poolIt != pools.end(); poolIt++)
    {
      ret = getObjectsFromCluster((*poolIt).get(), (*dataMapIt).first,
                                  mInodes);

      if (ret != 0)
        return ret;
    }
  }

  std::set<std::string>::const_iterator setIt;
  for (setIt = prefixes.begin(); setIt != prefixes.end(); setIt++)
  {
    checkDirRecursive(*setIt);
  }

  mBrokenInodes = mInodes;

  return 0;
}

int
RadosFsChecker::fixDirs()
{
  int ret = 0;
  std::map<std::string, std::string>::const_iterator it;

  for (it = mBrokenDirs.begin(); it != mBrokenDirs.end(); it++)
  {
    const std::string &path = (*it).first.c_str();

    if (mDry)
    {
      fprintf(stdout, "Would create %s\n", path.c_str());
    }
    else
    {
      radosfs::RadosFsDir dir(mRadosFs, path.c_str());

      if ((ret = dir.create()) != 0)
      {
        fprintf(stderr, "Error creating directory %s: %s."
                "Stopping the fixing...\n", path.c_str(),
                strerror(ret));

        return ret;
      }

      fprintf(stdout, "Created %s\n", dir.path().c_str());
    }
  }

  for (it = mDirs.begin(); it != mDirs.end(); it++)
  {
    const std::string &path = (*it).first.c_str();

    if (mDry)
    {
      fprintf(stdout, "Would index %s\n", path.c_str());
    }
    else
    {
      RadosFsStat stat;

      mRadosFs->mPriv->stat(path, &stat);


      if ((ret = indexObject(&stat, '+')) != 0)
      {
        fprintf(stderr, "Error indexing %s: %s."
                "Stopping the fixing...\n", stat.path.c_str(),
                strerror(ret));

        return ret;
      }

      fprintf(stdout, "Indexed %s\n", stat.path.c_str());
    }
  }

  return ret;
}

int
RadosFsChecker::fixInodes()
{
  std::map<std::string, std::string>::const_iterator it;
  for (it = mInodes.begin(); it != mInodes.end(); it++)
  {
    radosfs::RadosFsPoolListMap &poolMap = mRadosFs->mPriv->poolMap;
    radosfs::RadosFsPoolListMap::iterator poolIt;
    const std::string &inode = (*it).first;
    char hardLink[XATTR_LINK_LENGTH + 1];
    hardLink[0] = '\0';

    // retrieve the inode hard link from the correct pool
    for (poolIt = poolMap.begin(); poolIt != poolMap.end(); poolIt++)
    {
      const radosfs::RadosFsPoolList &poolList = (*poolIt).second;
      radosfs::RadosFsPoolList::const_iterator poolListIt;

      for (poolListIt = poolList.begin();
           poolListIt != poolList.end();
           poolListIt++)
      {
        int length = rados_getxattr((*poolListIt)->ioctx, inode.c_str(),
                                    XATTR_INODE_HARD_LINK, hardLink,
                                    XATTR_FILE_LENGTH);
        if (length >= 0)
        {
          hardLink[length] = '\0';
          break;
        }
      }

      if (hardLink[0] != '\0')
        break;
    }

    std::string action;
    if (strlen(hardLink) > 0)
    {
      if (mDry)
      {
        action = "Would create";
      }
      else
      {
        action = "Created";

        std::tr1::shared_ptr<RadosFsPool> pool;
        RadosFsStat stat;

        pool = mRadosFs->mPriv->getMetadataPoolFromPath(hardLink);

        if (!pool.get())
        {
          fprintf(stderr, "Failed to get metadata pool for %s (to point to "
                  "%s)\n", hardLink, inode.c_str());

          return -ENODEV;
        }

        stat.path = hardLink;
        stat.pool = pool;
        // We just need it to be a file. The permissions are already
        // set on the inode
        stat.statBuff.st_mode = S_IFREG;
        stat.translatedPath = inode;

        indexObject(&stat, '+');
      }

      fprintf(stdout, "%s %s (pointing to %s)\n", action.c_str(), hardLink,
              inode.c_str());
    }
    else
    {
      if (mDry)
      {
        action = "Would ignore this inode...";
      }
      else
      {
        action = "Ignored this inode...";
      }

      fprintf(stderr, "Cannot find the file path linking to %s. %s\n",
              inode.c_str(), action.c_str());
    }
  }

  return 0;
}

void
printSet(const std::set<std::string> &set)
{
  if (set.size() == 0)
    return;

  std::set<std::string>::const_iterator it;

  for (it = set.begin(); it != set.end(); it++)
  {
    fprintf(stdout, " %s\n", (*it).c_str());
  }

  fprintf(stdout, "---------------\n");
}

void
printMap(const std::map<std::string, std::string> &map,
         const std::string &valuePrefix)
{
  if (map.size() == 0)
    return;

  std::map<std::string, std::string>::const_iterator it;

  for (it = map.begin(); it != map.end(); it++)
  {
    if (valuePrefix != "")
      fprintf(stdout, " %s\t\t%s%s\n", (*it).first.c_str(),
              valuePrefix.c_str(), (*it).second.c_str());
    else
      fprintf(stdout, " %s\n", (*it).first.c_str());
  }

  fprintf(stdout, "---------------\n");
}

void
RadosFsChecker::printIssues(void)
{
  size_t totalIssues = mBrokenDirs.size() + mBrokenInodes.size() +
                       mBrokenFiles.size() + mBrokenLinks.size() +
                       mInodes.size() + mDirs.size();

  fprintf(stdout, "\nTotal issues found: %lu\n\n", totalIssues);

  if (totalIssues == 0)
    return;

  fprintf(stdout, "Indexed but missing dirs: %lu\n", mBrokenDirs.size());

  if (mVerbose)
    printMap(mBrokenDirs, "");

  fprintf(stdout, "Existing but unindexed dirs: %lu\n", mBrokenDirs.size());

  if (mVerbose)
    printMap(mDirs, "");

  fprintf(stdout, "Inodes without a file: %lu\n", mBrokenInodes.size());

  if (mVerbose)
    printMap(mBrokenInodes, "pool=");

  fprintf(stdout, "Files pointing to unexisting inodes: %lu\n",
          mBrokenFiles.size());

  if (mVerbose)
    printSet(mBrokenFiles);

  fprintf(stdout, "Symbolic links to unexisting files/dirs: %lu\n",
          mBrokenLinks.size());

  if (mVerbose)
    printMap(mBrokenLinks, "target=");
}

void
RadosFsChecker::fix(void)
{
  check();

  if (mDirs.size() == 0 && mInodes.size() == 0)
  {
    fprintf(stdout, "Nothing to fix. No unindexed or nonexistent directories "
            "and no inodes without pointers.");
    return;
  }

  fixDirs();
  fixInodes();
}