/**
 *
 */
package org.openj9.test.jep401;

public class Sample {
    public static final void sub(Object[] arr) {
        arr[0] = null;
    }

    public static final void main(String[] args) {
        for (int i = 0; i < 10000000; i++) {
            sub(new Object[1]);
        }
    }
}
