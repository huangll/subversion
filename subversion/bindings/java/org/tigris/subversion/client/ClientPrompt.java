package org.tigris.subversion.client;

/*
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

import org.tigris.subversion.SubversionException;
import org.tigris.subversion.auth.AuthProvider;

/**
 * The methods of this interface correspond to the types and functions
 * described in the subversion C api located in 'svn_client.h'.
 */
public interface ClientPrompt
{
    /**
     * @param prompt The application's query to the user.
     * @param hide Whether to display the user's answer to
     * <code>prompt</code>
     * @return The response to <code>prompt</code>.
     */
    String[] prompt(String prompt, boolean hide)
        throws SubversionException;
}
