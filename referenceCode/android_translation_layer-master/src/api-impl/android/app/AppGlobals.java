package android.app;

import android.content.Context;

public class AppGlobals {

	public static Application getInitialApplication() {
		return Context.this_application;
	}
}
