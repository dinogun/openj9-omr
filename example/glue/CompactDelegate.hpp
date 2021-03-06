/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2017
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#ifndef COMPACTDELEGATE_HPP_
#define COMPACTDELEGATE_HPP_

#include "omrcfg.h" 
#include "omrgcconsts.h"

class MM_CompactScheme;
class MM_EnvironmentBase; 
class MM_MarkMap;

#if defined(OMR_GC_MODRON_COMPACTION)

/**
 * Delegate class provides implementations for methods required for Collector Language Interface
 */
class MM_CompactDelegate
{
	/*
	 * Data members
	 */
private:
	OMR_VM *_omrVM;
	MM_CompactScheme *_compactScheme;
	MM_MarkMap *_markMap;

protected:

public:

	/*
	 * Function members
	 */
private:

protected:

public:
	/**
	 * Initialize the delegate.
	 *
	 * @param omrVM
	 * @return true if delegate initialized successfully
	 */
	void
	tearDown(MM_EnvironmentBase *env) { }

	bool
	initialize(MM_EnvironmentBase *env, OMR_VM *omrVM, MM_MarkMap *markMap, MM_CompactScheme *compactScheme)
	{
		_omrVM  = omrVM;
		_compactScheme = compactScheme;
		_markMap = markMap;
		return true;
	}

	void
	verifyHeap(MM_EnvironmentBase *env, MM_MarkMap *markMap) { }

	void
	fixupRoots(MM_EnvironmentBase *env, MM_CompactScheme *compactScheme) { }

	void
	workerCleanupAfterGC(MM_EnvironmentBase *env) { }

	void
	masterSetupForGC(MM_EnvironmentBase *env) { }

	MM_CompactDelegate()
		: _compactScheme(NULL)
		, _markMap(NULL)
		, _omrVM(NULL)
	{}
};

#endif /* OMR_GC_MODRON_COMPACTION */
#endif /* COMPACTDELEGATE_HPP_ */
