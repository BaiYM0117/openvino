#
# softmax paddle model generator
#
import numpy as np
from save_model import saveModel


def softmax(name: str, x, axis):
    import paddle as pdpd
    pdpd.enable_static()

    node_x = pdpd.static.data(name='x', shape=x.shape, dtype='float32')
    out = pdpd.nn.functional.softmax(x=node_x, axis=axis)

    cpu = pdpd.static.cpu_places(1)
    exe = pdpd.static.Executor(cpu[0])
    # startup program will call initializer to initialize the parameters.
    exe.run(pdpd.static.default_startup_program())

    outs = exe.run(
        feed={'x': x},
        fetch_list=[out])

    saveModel(name, exe, feedkeys=['x'], fetchlist=[out], inputs=[x], outputs=[outs[0]])

    return outs[0]


def main():
    data = np.array(
        [[[2.0, 3.0, 4.0, 5.0],
          [3.0, 4.0, 5.0, 6.0],
          [7.0, 8.0, 8.0, 9.0]],
         [[1.0, 2.0, 3.0, 4.0],
          [5.0, 6.0, 7.0, 8.0],
          [6.0, 7.0, 8.0, 9.0]]]
    ).astype(np.float32)

    softmax("softmax", data, axis=1)


if __name__ == "__main__":
    main()